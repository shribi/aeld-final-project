/*
 * Userspace unit tests for the INMP frame parser.
 * Build and run: make test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../inmp-module/inmp_parser.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("  FAIL line %d: %s\n", __LINE__, #cond); \
		failures++; \
	} \
} while (0)

struct sink {
	uint8_t data[1 << 16];
	size_t len;
};

static void collect(void *ctx, const uint8_t *pcm, size_t len)
{
	struct sink *s = ctx;

	if (s->len + len <= sizeof(s->data)) {
		memcpy(s->data + s->len, pcm, len);
		s->len += len;
	}
}

/* Build one frame into out; returns its length. */
static size_t make_frame(uint8_t *out, uint16_t seq, const uint8_t *pcm,
			 uint16_t len)
{
	uint16_t crc;

	out[0] = INMP_MAGIC0; out[1] = INMP_MAGIC1;
	out[2] = INMP_MAGIC2; out[3] = INMP_MAGIC3;
	out[4] = seq & 0xFF;  out[5] = seq >> 8;
	out[6] = len & 0xFF;  out[7] = len >> 8;
	memcpy(out + 8, pcm, len);
	crc = inmp_crc16(0xFFFF, out + 4, 4 + len);
	out[8 + len] = crc & 0xFF;
	out[9 + len] = crc >> 8;
	return 10u + len;
}

static void fill(uint8_t *pcm, size_t len, uint8_t base)
{
	for (size_t i = 0; i < len; i++)
		pcm[i] = (uint8_t)(base + i);
}

static void setup(struct inmp_parser *p, struct sink *s)
{
	s->len = 0;
	inmp_parser_init(p, collect, s);
}

static void test_clean(void)
{
	struct inmp_parser p; struct sink s;
	uint8_t pcm[64], f[128];
	size_t n;

	puts("clean stream");
	setup(&p, &s);
	fill(pcm, sizeof pcm, 1);
	for (uint16_t i = 0; i < 5; i++) {
		n = make_frame(f, i, pcm, sizeof pcm);
		inmp_parser_feed(&p, f, n);
	}
	CHECK(p.stats.frames_ok == 5);
	CHECK(s.len == 5 * sizeof pcm);
	CHECK(memcmp(s.data, pcm, sizeof pcm) == 0);
	CHECK(p.stats.lost_frames == 0 && p.stats.crc_errors == 0);
}

static void test_bytewise(void)
{
	struct inmp_parser p; struct sink s;
	uint8_t pcm[512], f[600];
	size_t n;

	puts("byte-at-a-time feed, max payload");
	setup(&p, &s);
	fill(pcm, sizeof pcm, 7);
	n = make_frame(f, 0, pcm, sizeof pcm);
	for (size_t i = 0; i < n; i++)
		inmp_parser_feed(&p, f + i, 1);
	CHECK(p.stats.frames_ok == 1);
	CHECK(s.len == sizeof pcm && memcmp(s.data, pcm, sizeof pcm) == 0);
}

static void test_garbage_prefix(void)
{
	struct inmp_parser p; struct sink s;
	uint8_t pcm[32], buf[128];
	uint8_t junk[] = { 0x00, 0xA5, 0x5A, 0xA5, 0xA5, 0x5A, 0xC3, 0x11 };
	size_t n;

	puts("garbage and false magic before first frame");
	setup(&p, &s);
	fill(pcm, sizeof pcm, 3);
	memcpy(buf, junk, sizeof junk);
	n = sizeof junk + make_frame(buf + sizeof junk, 0, pcm, sizeof pcm);
	inmp_parser_feed(&p, buf, n);
	CHECK(p.stats.frames_ok == 1);
	CHECK(s.len == sizeof pcm && memcmp(s.data, pcm, sizeof pcm) == 0);
}

static void test_lost_frames(void)
{
	struct inmp_parser p; struct sink s;
	uint8_t pcm[64], f[128];
	size_t n;

	puts("lost frames are zero-filled");
	setup(&p, &s);
	fill(pcm, sizeof pcm, 1);
	n = make_frame(f, 10, pcm, sizeof pcm);
	inmp_parser_feed(&p, f, n);
	n = make_frame(f, 13, pcm, sizeof pcm); /* 11 and 12 lost */
	inmp_parser_feed(&p, f, n);
	CHECK(p.stats.lost_frames == 2);
	CHECK(s.len == 4 * sizeof pcm);
	for (size_t i = sizeof pcm; i < 3 * sizeof pcm; i++)
		CHECK(s.data[i] == 0);
}

static void test_seq_wrap(void)
{
	struct inmp_parser p; struct sink s;
	uint8_t pcm[16], f[64];
	size_t n;

	puts("sequence wraparound is not a loss");
	setup(&p, &s);
	fill(pcm, sizeof pcm, 1);
	n = make_frame(f, 0xFFFF, pcm, sizeof pcm);
	inmp_parser_feed(&p, f, n);
	n = make_frame(f, 0, pcm, sizeof pcm);
	inmp_parser_feed(&p, f, n);
	CHECK(p.stats.lost_frames == 0 && p.stats.frames_ok == 2);
}

static void test_huge_gap(void)
{
	struct inmp_parser p; struct sink s;
	uint8_t pcm[16], f[64];
	size_t n;

	puts("huge gap and reordering are not zero-filled");
	setup(&p, &s);
	fill(pcm, sizeof pcm, 1);
	n = make_frame(f, 100, pcm, sizeof pcm);
	inmp_parser_feed(&p, f, n);
	n = make_frame(f, 5000, pcm, sizeof pcm);
	inmp_parser_feed(&p, f, n);
	n = make_frame(f, 50, pcm, sizeof pcm);
	inmp_parser_feed(&p, f, n);
	CHECK(p.stats.lost_frames == 0);
	CHECK(p.stats.seq_jumps == 2);
	CHECK(s.len == 3 * sizeof pcm);
}

static void test_crc_error(void)
{
	struct inmp_parser p; struct sink s;
	uint8_t pcm[64], f[128];
	size_t n;

	puts("corrupted frame dropped, stream recovers");
	setup(&p, &s);
	fill(pcm, sizeof pcm, 1);
	n = make_frame(f, 0, pcm, sizeof pcm);
	inmp_parser_feed(&p, f, n);
	n = make_frame(f, 1, pcm, sizeof pcm);
	f[20] ^= 0xFF;
	inmp_parser_feed(&p, f, n);
	n = make_frame(f, 2, pcm, sizeof pcm);
	inmp_parser_feed(&p, f, n);
	CHECK(p.stats.crc_errors >= 1);
	CHECK(p.stats.frames_ok == 2);
	CHECK(p.stats.lost_frames == 1);
	CHECK(p.stats.resyncs == 1);
	CHECK(s.len == 3 * sizeof pcm);
}

static void test_magic_in_payload(void)
{
	struct inmp_parser p; struct sink s;
	uint8_t pcm[64], inner[64], f[256], g[128];
	size_t n, m;

	puts("frame embedded inside a corrupted frame's payload is found");
	setup(&p, &s);
	fill(pcm, sizeof pcm, 9);
	fill(inner, sizeof inner, 1);
	m = make_frame(g, 1, inner, sizeof inner);
	/* outer frame whose payload is 10 bytes + a whole valid frame */
	memset(pcm, 0x11, sizeof pcm);
	memcpy(pcm + 2, g, 62 < m ? 62 : m); /* truncated copy, no valid inner */
	n = make_frame(f, 0, pcm, sizeof pcm);
	f[n - 1] ^= 0x01; /* break outer CRC */
	inmp_parser_feed(&p, f, n);
	m = make_frame(g, 1, inner, sizeof inner);
	inmp_parser_feed(&p, g, m);
	CHECK(p.stats.frames_ok == 1);
}

static void test_bad_len(void)
{
	struct inmp_parser p; struct sink s;
	uint8_t pcm[32], f[128], bad[8] = {
		INMP_MAGIC0, INMP_MAGIC1, INMP_MAGIC2, INMP_MAGIC3,
		0, 0, 0xFF, 0xFF };
	size_t n;

	puts("oversized length rejected without stalling");
	setup(&p, &s);
	fill(pcm, sizeof pcm, 1);
	inmp_parser_feed(&p, bad, sizeof bad);
	n = make_frame(f, 0, pcm, sizeof pcm);
	inmp_parser_feed(&p, f, n);
	CHECK(p.stats.bad_len >= 1);
	CHECK(p.stats.frames_ok == 1);
}

static void test_random_noise(void)
{
	struct inmp_parser p; struct sink s;
	uint8_t pcm[64], f[128], noise[4096];
	size_t n;

	puts("random noise never crashes and stream recovers");
	setup(&p, &s);
	srand(1234);
	for (size_t i = 0; i < sizeof noise; i++)
		noise[i] = (uint8_t)rand();
	inmp_parser_feed(&p, noise, sizeof noise);
	fill(pcm, sizeof pcm, 5);
	n = make_frame(f, 0, pcm, sizeof pcm);
	inmp_parser_feed(&p, f, n);
	CHECK(p.stats.frames_ok >= 1);
	CHECK(memcmp(s.data + s.len - sizeof pcm, pcm, sizeof pcm) == 0);
}

int main(void)
{
	test_clean();
	test_bytewise();
	test_garbage_prefix();
	test_lost_frames();
	test_seq_wrap();
	test_huge_gap();
	test_crc_error();
	test_magic_in_payload();
	test_bad_len();
	test_random_noise();

	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
