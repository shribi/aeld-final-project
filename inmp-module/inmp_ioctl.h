/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Shared between the INMP441 line discipline (kernel) and userspace.
 * Issue these on the tty fd (e.g. /dev/ttyUSB0) after TIOCSETD.
 */
#ifndef INMP_IOCTL_H
#define INMP_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define INMP_IOC_MAGIC	'I'

/* 1: flush the FIFO, reset the parser, start queueing PCM */
#define INMP_IOC_START		_IO(INMP_IOC_MAGIC, 1)
/* 2: stop queueing; read() drains what is left, then returns 0 */
#define INMP_IOC_STOP		_IO(INMP_IOC_MAGIC, 2)
/* 3: fetch counters */
#define INMP_IOC_GET_STATS	_IOR(INMP_IOC_MAGIC, 3, struct inmp_ioc_stats)

struct inmp_ioc_stats {
	__u64 rx_bytes;		/* raw bytes seen from the tty since attach */
	__u64 read_bytes;	/* PCM bytes returned by read() since START */
	__u32 frames_ok;
	__u32 lost_frames;
	__u32 crc_errors;
	__u32 bad_len;
	__u32 resyncs;
	__u32 bytes_discarded;
	__u32 seq_jumps;
	__u32 overrun_bytes;	/* PCM bytes dropped because the FIFO was full */
};

#endif /* INMP_IOCTL_H */
