/* SPDX-License-Identifier: GPL-3.0-only   */
/*
 * Robustness test for the parcel filter: random bytes, valid-looking lists and
 * every length around the parsers' boundaries. Compiled with ASan/UBSan in CI
 * (see the Makefile), so a single out-of-bounds read in the kernel parser fails
 * the run instead of becoming a boot loop on a phone.
 *
 * Deterministic: the seed is fixed, so a failure is reproducible.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xechide.h"

#define BUF_SIZE 2048
#define ITERATIONS 200000

static unsigned int rng_state = 0x13579bdfu;

static unsigned int next_rand(void)
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return rng_state >> 8;
}

static const char *const names[] = {
	"com.example.hidden",
	"a",
	"com.example.hidden.with.a.very.long.name.that.is.still.under.the.limit",
	"com.example.visible",
};

static void fill_random(unsigned char *buf, unsigned int len)
{
	unsigned int i;

	for (i = 0; i < len; i++)
		buf[i] = (unsigned char)next_rand();
}

/* Build a structurally valid List<PackageInfo> with the given item count. */
static unsigned int build_pkg_list(unsigned char *buf, unsigned int items, unsigned int enc16)
{
	unsigned int off = 0, i, j;

	memcpy(buf + off, &(unsigned int){ 0 }, 4);
	off += 4;
	memcpy(buf + off, &items, 4);
	off += 4;
	for (i = 0; i < items; i++) {
		const char *name = names[i % 4];
		unsigned int n = (unsigned int)strlen(name);
		unsigned int total = ((4u + (enc16 ? 2u : 1u) * n + (enc16 ? 2u : 1u)) + 3u) & ~3u;
		unsigned int one = 1;

		memcpy(buf + off, &one, 4);
		off += 4;
		memcpy(buf + off, &n, 4);
		if (enc16) {
			for (j = 0; j < n; j++) {
				buf[off + 4 + 2 * j] = (unsigned char)name[j];
				buf[off + 5 + 2 * j] = 0;
			}
		} else {
			for (j = 0; j < n; j++)
				buf[off + 4 + j] = (unsigned char)name[j];
		}
		off += total;
		/* opaque body */
		for (j = 0; j < 12; j++)
			buf[off++] = (unsigned char)(0x40 + j);
	}
	return off;
}

/* Build a structurally valid String[] with the given item count. */
static unsigned int build_string_array(unsigned char *buf, unsigned int items)
{
	unsigned int off = 0, i, j;

	memcpy(buf + off, &(unsigned int){ 0 }, 4);
	off += 4;
	memcpy(buf + off, &items, 4);
	off += 4;
	for (i = 0; i < items; i++) {
		const char *name = names[i % 4];
		unsigned int n = (unsigned int)strlen(name);
		unsigned int total = ((4u + 2u * n + 2u) + 3u) & ~3u;

		memcpy(buf + off, &n, 4);
		for (j = 0; j < n; j++) {
			buf[off + 4 + 2 * j] = (unsigned char)name[j];
			buf[off + 5 + 2 * j] = 0;
		}
		off += total;
	}
	return off;
}

int main(void)
{
	static unsigned char buf[BUF_SIZE + 64];
	unsigned int i;

	for (i = 0; i < ITERATIONS; i++) {
		unsigned int mode = 0, len, new_len, kind = next_rand() % 4;

		memset(buf, 0, sizeof(buf));
		if (kind == 0) {
			len = 8 + next_rand() % (BUF_SIZE - 8);
			fill_random(buf, len);
		} else if (kind == 1) {
			len = build_pkg_list(buf, next_rand() % 6, next_rand() & 1);
		} else if (kind == 2) {
			len = build_string_array(buf, next_rand() % 6);
		} else {
			len = 8 + next_rand() % 256;
			fill_random(buf, len);
			buf[0] = 0;
			buf[4] = (unsigned char)(next_rand() % 6);
		}
		if (len < 8 || len > BUF_SIZE)
			continue;

		new_len = xe_parcel_filter(buf, len, names, 4, &mode);
		if (new_len > len) {
			printf("FAIL: filter grew the parcel (%u > %u)\n", new_len, len);
			return 1;
		}
		if (mode == XE_PARCEL_REBUILT && new_len == 0) {
			printf("FAIL: rebuilt without a length\n");
			return 1;
		}
	}

	/* A few explicit edge cases the generators do not reach. */
	{
		unsigned int mode = 0;
		unsigned int len = build_pkg_list(buf, 0, 1);

		(void)xe_parcel_filter(buf, len, names, 4, &mode);
		len = build_string_array(buf, 0);
		(void)xe_parcel_filter(buf, len, names, 4, &mode);
		(void)xe_parcel_filter(buf, 8, names, 0, &mode);
		(void)xe_parcel_filter(buf, 8, names, -1, &mode);
	}

	printf("fuzz: %d iterations survived\n", ITERATIONS);
	return 0;
}
