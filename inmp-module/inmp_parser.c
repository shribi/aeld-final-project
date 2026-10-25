// SPDX-License-Identifier: GPL-2.0
#ifdef __KERNEL__
#include <linux/string.h>
#include <linux/printk.h>
#endif
#include "inmp_parser.h"

#ifndef __KERNEL__
#include <string.h>
#endif

static const uint8_t magic[INMP_MAGIC_LEN] = {
	INMP_MAGIC0, INMP_MAGIC1, INMP_MAGIC2, INMP_MAGIC3
};

static const uint8_t zeros[INMP_MAX_PAYLOAD];

uint16_t inmp_crc16(uint16_t crc, const uint8_t *d, size_t n)
{
	while (n--) {
		int i;

		crc ^= (uint16_t)(*d++) << 8;
		for (i = 0; i < 8; i++)
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
					     : (uint16_t)(crc << 1);
	}
	return crc;
}

void inmp_parser_init(struct inmp_parser *p, inmp_emit_fn emit, void *ctx)
{
	memset(p, 0, sizeof(*p));
	p->emit = emit;
	p->ctx = ctx;
	p->last_len = INMP_MAX_PAYLOAD;
}

/* Drop k bytes from the front of the buffer. */
static void consume(struct inmp_parser *p, size_t k)
{
	memmove(p->buf, p->buf + k, p->have - k);
	p->have -= k;
}

/* Drop k bytes because they could not be parsed. */
static void discard(struct inmp_parser *p, size_t k)
{
	p->stats.bytes_discarded += k;
	if (p->in_sync) {
		p->in_sync = 0;
		p->stats.resyncs++;
	}
	consume(p, k);
}

/* Drop bytes until the buffer starts with a (possibly partial) magic. */
static void sync_to_magic(struct inmp_parser *p)
{
	size_t i = 0;

	while (i < p->have) {
		size_t m = p->have - i;

		if (m > INMP_MAGIC_LEN)
			m = INMP_MAGIC_LEN;
		if (p->buf[i] == magic[0] && memcmp(p->buf + i, magic, m) == 0)
			break;
		i++;
	}
	if (i)
		discard(p, i);
}

static void emit_zeros(struct inmp_parser *p, size_t len)
{
	while (len) {
		size_t n = len > sizeof(zeros) ? sizeof(zeros) : len;

		p->emit(p->ctx, zeros, n);
		len -= n;
	}
}

static void deliver(struct inmp_parser *p, uint16_t seq, uint16_t len)
{
	if (p->have_seq) {
		uint16_t gap = (uint16_t)(seq - p->expected_seq);

		if (gap && gap <= INMP_MAX_GAP_FRAMES) {
			p->stats.lost_frames += gap;
			emit_zeros(p, (size_t)gap * p->last_len);
		} else if (gap) {
			/* Huge jump or reordering: do not fabricate audio. */
			p->stats.seq_jumps++;
		}
	}
	p->have_seq = 1;
	p->expected_seq = seq + 1;
	p->last_len = len;
	p->in_sync = 1;
	p->stats.frames_ok++;
	p->emit(p->ctx, p->buf + INMP_HDR_LEN, len);
}

static void process(struct inmp_parser *p)
{
	for (;;) {
		uint16_t seq, len, crc, want;
		size_t total;

		sync_to_magic(p);
		if (p->have < INMP_HDR_LEN)
			return;

		seq = p->buf[4] | (p->buf[5] << 8);
		len = p->buf[6] | (p->buf[7] << 8);
		if (len == 0 || len > INMP_MAX_PAYLOAD || (len & 1)) {
			p->stats.bad_len++;
			discard(p, 1);
			continue;
		}

		total = INMP_HDR_LEN + len + INMP_CRC_LEN;
		if (p->have < total)
			return;

		crc = inmp_crc16(0xFFFF, p->buf + 4, 4 + len);
		want = p->buf[INMP_HDR_LEN + len] |
		       (p->buf[INMP_HDR_LEN + len + 1] << 8);
		if (crc != want) {
			/* Skip one byte only: a real frame may start inside. */
			p->stats.crc_errors++;
			discard(p, 1);
			continue;
		}

		pr_info_ratelimited("inmp441: frame ok: seq=%u len=%u crc=%04x\n", seq, len, crc);

		deliver(p, seq, len);
		consume(p, total);
	}
}

void inmp_parser_feed(struct inmp_parser *p, const uint8_t *data, size_t len)
{
	while (len) {
		size_t room = sizeof(p->buf) - p->have;
		size_t n = len < room ? len : room;

		memcpy(p->buf + p->have, data, n);
		p->have += n;
		data += n;
		len -= n;
		process(p);
	}
}
