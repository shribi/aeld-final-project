// SPDX-License-Identifier: GPL-2.0
/*
 * INMP441 line discipline.
 *
 * Attach from userspace: open the tty, set raw 8N1 + baud, then
 *     ioctl(fd, TIOCSETD, &ldisc)      (ldisc = 29, N_DEVELOPMENT)
 * Then:
 *     ioctl(fd, INMP_IOC_START)        flush + begin queueing PCM
 *     read(fd, ...)                    clean 16 kHz 16-bit mono PCM
 *     ioctl(fd, INMP_IOC_STOP)         stop queueing; read() drains, then 0
 *
 * Frames from the ESP32 are decoded by inmp_parser in receive_buf2().
 * The parser always runs (to stay in sync); PCM is only queued while
 * capturing.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/tty.h>
#include <linux/tty_ldisc.h>
#include <linux/kfifo.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/moduleparam.h>
#include <linux/minmax.h>
#include <linux/version.h>

#include "inmp_parser.h"
#include "inmp_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Srinidhi Bhat");
MODULE_DESCRIPTION("INMP441 serial line discipline");

#define INMP_LDISC_NAME  "inmp441"
#define INMP_FIFO_BYTES  65536	/* power of two; 2 s at 16 kHz 16-bit mono */
#define INMP_HEX_PREVIEW 16	/* bytes of PCM shown per logged read() */

/* Debug logging, applied at each INMP_IOC_START. */
static unsigned int log_reads = 16;
module_param(log_reads, uint, 0644);
MODULE_PARM_DESC(log_reads, "Log the first N read() calls after START, with a hex preview (0 = off)");

static unsigned int log_every = 65536;
module_param(log_every, uint, 0644);
MODULE_PARM_DESC(log_every, "Then log a progress line every N PCM bytes read (0 = off)");

/* Callback signatures changed in newer kernels (u8/size_t).
 * The 6.8 boundary is a guess: check include/linux/tty_ldisc.h on each
 * target kernel (6.1 and 7.0) and adjust. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
typedef u8 inmp_byte_t;
typedef u8 inmp_flag_t;
typedef size_t inmp_count_t;
#else
typedef unsigned char inmp_byte_t;
typedef char inmp_flag_t;
typedef int inmp_count_t;
#endif

struct inmp_dev {
	struct inmp_parser parser;	/* protected by lock */
	DECLARE_KFIFO_PTR(fifo, u8);	/* SPSC: producer = receive_buf2, consumer = read */
	wait_queue_head_t wq;

	spinlock_t lock;		/* capturing, parser, fifo reset, producer side */
	struct mutex read_lock;		/* serialises consumers and fifo reset */

	bool capturing;			/* written under lock, read with READ_ONCE */
	bool dead;			/* closed or hung up */

	u32 overrun_bytes;		/* under lock */
	u64 rx_bytes;			/* raw bytes from the tty, under lock */

	/* read-side bookkeeping, protected by read_lock */
	u64 read_bytes;
	u64 next_report;
	unsigned int log_budget;
};

/*
 * Parser output. Called from inmp_parser_feed() inside receive_buf2(),
 * with d->lock held. Must not sleep.
 */
static void inmp_emit(void *ctx, const uint8_t *pcm, size_t len)
{
	struct inmp_dev *d = ctx;

	if (!d->capturing)
		return;			/* idle: parser stays in sync, samples dropped */

	/* Whole 16-bit samples only, otherwise the stream goes out of phase. */
	if (len > (kfifo_avail(&d->fifo) & ~1u)) {
		d->overrun_bytes += len;
		return;
	}

	kfifo_in(&d->fifo, pcm, len);
	wake_up_interruptible(&d->wq);
}

static int inmp_ldisc_open(struct tty_struct *tty)
{
	struct inmp_dev *d;
	int ret;

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	ret = kfifo_alloc(&d->fifo, INMP_FIFO_BYTES, GFP_KERNEL);
	if (ret) {
		kfree(d);
		return ret;
	}
	init_waitqueue_head(&d->wq);
	spin_lock_init(&d->lock);
	mutex_init(&d->read_lock);
	inmp_parser_init(&d->parser, inmp_emit, d);

	tty->receive_room = 65536;	/* several ldiscs set this; harmless */
	tty->disc_data = d;
	pr_info("inmp441: attached to %s\n", tty->name);
	return 0;
}

static void inmp_ldisc_close(struct tty_struct *tty)
{
	struct inmp_dev *d = tty->disc_data;
	struct inmp_stats *s = &d->parser.stats;

	WRITE_ONCE(d->dead, true);
	wake_up_interruptible(&d->wq);

	/*
	 * Assumes the tty core does not run close() while a read() is still
	 * inside the ldisc (it should hold an ldisc reference across read).
	 * Verify in tty_io.c / tty_ldisc.c for 6.1 and 7.0.
	 */
	pr_info("inmp441: detached: rx_bytes=%llu read_bytes=%llu ok=%u lost=%u crc=%u bad_len=%u resync=%u discarded=%u jumps=%u overrun=%u\n",
		d->rx_bytes, d->read_bytes, s->frames_ok, s->lost_frames,
		s->crc_errors, s->bad_len, s->resyncs, s->bytes_discarded,
		s->seq_jumps, d->overrun_bytes);

	tty->disc_data = NULL;
	kfifo_free(&d->fifo);
	kfree(d);
}

static void inmp_ldisc_hangup(struct tty_struct *tty)
{
	struct inmp_dev *d = tty->disc_data;

	if (!d)
		return;
	WRITE_ONCE(d->dead, true);
	wake_up_interruptible(&d->wq);
}

static inmp_count_t inmp_ldisc_receive_buf2(struct tty_struct *tty,
					    const inmp_byte_t *cp,
					    const inmp_flag_t *fp,
					    inmp_count_t count)
{
	struct inmp_dev *d = tty->disc_data;
	unsigned long flags;
	bool first;

	/* Flags (parity/framing errors) are ignored; the CRC catches damage. */
	spin_lock_irqsave(&d->lock, flags);
	first = !d->rx_bytes;
	d->rx_bytes += count;
	inmp_parser_feed(&d->parser, cp, count);
	spin_unlock_irqrestore(&d->lock, flags);

	if (first)
		pr_info("inmp441: first %zu bytes received on %s\n",
			(size_t)count, tty->name);
	return count;
}

/* ---- control: START / STOP / GET_STATS ---------------------------------- */

static int inmp_start(struct inmp_dev *d)
{
	unsigned long flags;

	if (READ_ONCE(d->dead))
		return -EIO;

	/* read_lock first: nobody may be inside kfifo_out() while we reset it. */
	if (mutex_lock_interruptible(&d->read_lock))
		return -ERESTARTSYS;

	spin_lock_irqsave(&d->lock, flags);
	kfifo_reset(&d->fifo);
	/* Re-init clears the sequence baseline (no bogus zero-fill after idle)
	 * and also resets the stats: they are per recording. Assumes
	 * inmp_parser_init() zeroes the whole parser state. */
	inmp_parser_init(&d->parser, inmp_emit, d);
	d->overrun_bytes = 0;
	d->capturing = true;
	spin_unlock_irqrestore(&d->lock, flags);

	d->read_bytes = 0;
	d->log_budget = log_reads;
	d->next_report = log_every;
	mutex_unlock(&d->read_lock);

	pr_info("inmp441: START (ioctl 1): fifo flushed, parser reset, capturing; will log first %u reads, then every %u bytes\n",
		log_reads, log_every);
	return 0;
}

static int inmp_stop(struct inmp_dev *d)
{
	unsigned long flags;
	struct inmp_stats s;
	u32 overrun;

	spin_lock_irqsave(&d->lock, flags);
	d->capturing = false;
	s = d->parser.stats;
	overrun = d->overrun_bytes;
	spin_unlock_irqrestore(&d->lock, flags);

	wake_up_interruptible(&d->wq);	/* blocked readers see EOF once drained */

	pr_info("inmp441: STOP (ioctl 2): read_bytes=%llu ok=%u lost=%u crc=%u bad_len=%u resync=%u discarded=%u jumps=%u overrun=%u fifo_left=%u\n",
		d->read_bytes, s.frames_ok, s.lost_frames, s.crc_errors,
		s.bad_len, s.resyncs, s.bytes_discarded, s.seq_jumps,
		overrun, kfifo_len(&d->fifo));
	return 0;
}

static int inmp_get_stats(struct inmp_dev *d, void __user *argp)
{
	struct inmp_ioc_stats st = { };
	unsigned long flags;

	spin_lock_irqsave(&d->lock, flags);
	st.rx_bytes        = d->rx_bytes;
	st.frames_ok       = d->parser.stats.frames_ok;
	st.lost_frames     = d->parser.stats.lost_frames;
	st.crc_errors      = d->parser.stats.crc_errors;
	st.bad_len         = d->parser.stats.bad_len;
	st.resyncs         = d->parser.stats.resyncs;
	st.bytes_discarded = d->parser.stats.bytes_discarded;
	st.seq_jumps       = d->parser.stats.seq_jumps;
	st.overrun_bytes   = d->overrun_bytes;
	spin_unlock_irqrestore(&d->lock, flags);

	st.read_bytes = READ_ONCE(d->read_bytes);	/* informational, not locked */

	return copy_to_user(argp, &st, sizeof(st)) ? -EFAULT : 0;
}

/* Signature as in 6.1; verify against tty_ldisc.h on 7.0. */
static int inmp_ldisc_ioctl(struct tty_struct *tty, unsigned int cmd,
			    unsigned long arg)
{
	struct inmp_dev *d = tty->disc_data;

	if (!d)
		return -EIO;

	switch (cmd) {
	case INMP_IOC_START:
		return inmp_start(d);
	case INMP_IOC_STOP:
		return inmp_stop(d);
	case INMP_IOC_GET_STATS:
		return inmp_get_stats(d, (void __user *)arg);
	}
	return -ENOIOCTLCMD;	/* let the tty core handle everything else */
}

/* ---- read / poll --------------------------------------------------------- */

/*
 * The tty core gives us a kernel bounce buffer (I believe capped at about
 * 64 bytes per call; check iterate_tty_read() in tty_io.c), so use
 * kfifo_out(), not kfifo_to_user(). A user read() may return a short count.
 *
 * Returns: >0 bytes, 0 = EOF (stopped and drained, or hung up),
 *          -EAGAIN for O_NONBLOCK with nothing queued.
 */
static ssize_t inmp_ldisc_read(struct tty_struct *tty, struct file *file,
			       inmp_byte_t *buf, size_t nr, void **cookie,
			       unsigned long offset)
{
	struct inmp_dev *d = tty->disc_data;
	unsigned int got;
	ssize_t ret;

	if (!d)
		return -EIO;
	if (!nr)
		return 0;

	if (mutex_lock_interruptible(&d->read_lock))
		return -ERESTARTSYS;

	for (;;) {
		got = kfifo_out(&d->fifo, buf, nr);
        pr_debug("inmp441: read(): requested=%zu, got=%u, fifo_left=%u\n",
            nr, got, kfifo_len(&d->fifo));
		if (got) {
			d->read_bytes += got;
			if (d->log_budget) {
				d->log_budget--;
			} else if (log_every && d->read_bytes >= d->next_report) {
				d->next_report = d->read_bytes + log_every;
			}
            else if (log_every && d->read_bytes < d->next_report) {
                d->next_report = d->read_bytes + log_every;
            }
			ret = got;
			break;
		}

		/* Empty: EOF if stopped/idle or hung up, else wait. */
		if (READ_ONCE(d->dead) || !READ_ONCE(d->capturing)) {
			ret = 0;
			break;
		}
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			break;
		}

		mutex_unlock(&d->read_lock);
		if (wait_event_interruptible(d->wq,
					     !kfifo_is_empty(&d->fifo) ||
					     READ_ONCE(d->dead) ||
					     !READ_ONCE(d->capturing)))
			return -ERESTARTSYS;
		if (mutex_lock_interruptible(&d->read_lock))
			return -ERESTARTSYS;
	}

	mutex_unlock(&d->read_lock);
	return ret;
}

static __poll_t inmp_ldisc_poll(struct tty_struct *tty, struct file *file,
				struct poll_table_struct *wait)
{
	struct inmp_dev *d = tty->disc_data;
	__poll_t mask = 0;

	if (!d)
		return EPOLLERR;

	poll_wait(file, &d->wq, wait);
	if (!kfifo_is_empty(&d->fifo))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (READ_ONCE(d->dead))
		mask |= EPOLLHUP;
	return mask;
}

static struct tty_ldisc_ops inmp_ldisc_ops = {
	.owner		= THIS_MODULE,
	.num		= N_DEVELOPMENT,
	.name		= INMP_LDISC_NAME,
	.open		= inmp_ldisc_open,
	.close		= inmp_ldisc_close,
	.hangup		= inmp_ldisc_hangup,
	.read		= inmp_ldisc_read,
	.poll		= inmp_ldisc_poll,
	.ioctl		= inmp_ldisc_ioctl,
	.receive_buf2	= inmp_ldisc_receive_buf2,
};

static int __init inmp_module_init(void)
{
	int ret = tty_register_ldisc(&inmp_ldisc_ops);

	if (ret)
		pr_err("inmp441: tty_register_ldisc failed: %d\n", ret);
	else
		pr_info("inmp441: line discipline registered (N_DEVELOPMENT)\n");
	return ret;
}

static void __exit inmp_module_exit(void)
{
	tty_unregister_ldisc(&inmp_ldisc_ops);
	pr_info("inmp441: line discipline unregistered\n");
}

module_init(inmp_module_init);
module_exit(inmp_module_exit);
