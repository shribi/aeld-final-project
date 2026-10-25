/* SPDX-License-Identifier: GPL-2.0 */
/*
 * INMP441 serial frame parser. Pure function: bytes in, PCM out.
 * No allocation, no sleeping, no I/O: safe in kernel receive_buf()
 * context and in a userspace test harness.
 *
 * Wire format (matches firmware/src/main.cpp), all fields little-endian:
 *
 *   magic[4]  A5 5A C3 3C
 *   seq       u16   increments per frame, wraps
 *   len       u16   payload bytes (even, 2..INMP_MAX_PAYLOAD)
 *   payload   len bytes of 16-bit LE mono PCM
 *   crc       u16   CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF)
 *                   over seq, len and payload
 */
#ifndef INMP_PARSER_H
#define INMP_PARSER_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

#define INMP_MAGIC0        0xA5
#define INMP_MAGIC1        0x5A
#define INMP_MAGIC2        0xC3
#define INMP_MAGIC3        0x3C
#define INMP_MAGIC_LEN     4
#define INMP_HDR_LEN       8   /* magic + seq + len */
#define INMP_CRC_LEN       2
#define INMP_MAX_PAYLOAD   1024
#define INMP_MAX_FRAME     (INMP_HDR_LEN + INMP_MAX_PAYLOAD + INMP_CRC_LEN)

/* Largest run of lost frames that is zero-filled; larger jumps resync. */
#define INMP_MAX_GAP_FRAMES 8

struct inmp_stats {
	uint32_t frames_ok;
	uint32_t lost_frames;     /* frames inferred missing from seq gaps */
	uint32_t crc_errors;
	uint32_t bad_len;
	uint32_t resyncs;         /* times sync was lost after a good frame */
	uint32_t bytes_discarded;
	uint32_t seq_jumps;       /* gaps too large to fill (or reordering) */
};

/* Called with validated PCM, and with zeros for lost frames. */
typedef void (*inmp_emit_fn)(void *ctx, const uint8_t *pcm, size_t len);

struct inmp_parser {
	uint8_t buf[INMP_MAX_FRAME];
	size_t have;
	uint16_t expected_seq;
	uint16_t last_len;
	uint8_t have_seq;
	uint8_t in_sync;
	inmp_emit_fn emit;
	void *ctx;
	struct inmp_stats stats;
};

uint16_t inmp_crc16(uint16_t crc, const uint8_t *data, size_t len);
void inmp_parser_init(struct inmp_parser *p, inmp_emit_fn emit, void *ctx);
void inmp_parser_feed(struct inmp_parser *p, const uint8_t *data, size_t len);

#endif /* INMP_PARSER_H */
