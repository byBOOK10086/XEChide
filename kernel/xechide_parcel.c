/* SPDX-License-Identifier: GPL-2.0 */
/*
 * XEChide - binder parcel rewriting.
 *
 * Every decision is made on a private, bounds-checked copy of the parcel. Two
 * strategies exist, in order of preference:
 *
 *   1. structural removal - the parcel is a `List<PackageInfo>` or a
 *      `String[]` and every element boundary can be proven from the parcel
 *      itself. The hidden element is removed and the container count updated.
 *   2. in-place substitution - a length-preserving rename of the hidden
 *      package name. The parcel layout does not change at all, so this cannot
 *      desynchronise the receiver.
 *
 * Anything that does not validate is left untouched (the caller keeps the
 * original parcel). No allocation happens here.
 */
#include "xechide.h"

#ifdef XECHIDE_HOST
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#else
#include <linux/string.h>
#include <linux/types.h>
#endif

#define XE_MAX_ITEMS 512
#define XE_MAX_NAMES 16
#define XE_MAX_NAME_LEN 127

#define XE_ALIGN4(x) (((x) + 3u) & ~3u)

struct xe_name {
	const char *p;
	unsigned int len;
};

struct xe_item {
	unsigned int off;      /* element start */
	unsigned int name_off; /* package name bytes */
	unsigned int name_len;
	unsigned int end; /* filled in once the next element is known */
};

/*
 * Scratch, not stack: a `List<PackageInfo>` reply can carry hundreds of
 * elements and a kernel stack cannot afford 8 KiB. The binder glue serialises
 * every call with its scratch-buffer lock, so a single instance is enough.
 */
static struct xe_item xe_items[XE_MAX_ITEMS];
static int xe_hidden[XE_MAX_ITEMS];

static inline unsigned int rd32(const unsigned char *p)
{
	unsigned int v;

	memcpy(&v, p, 4);
	return v;
}

static inline int rds32(const unsigned char *p)
{
	int v;

	memcpy(&v, p, 4);
	return v;
}

static inline void wr32(unsigned char *p, unsigned int v)
{
	memcpy(p, &v, 4);
}

static bool zeros(const unsigned char *p, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		if (p[i])
			return false;
	return true;
}

/* 4 (length) + 2n (chars) + 2 (nul), 4-byte aligned */
static unsigned int str16_total(unsigned int n)
{
	return XE_ALIGN4(4u + 2u * n + 2u);
}

/* 4 (length) + n (chars) + 1 (nul), 4-byte aligned */
static unsigned int str8_total(unsigned int n)
{
	return XE_ALIGN4(4u + n + 1u);
}

static inline bool name_char(unsigned char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
	       c == '_';
}

static bool parcel_name_ok(const unsigned char *p, unsigned int n)
{
	unsigned int i;

	if (n == 0 || n > XE_MAX_NAME_LEN)
		return false;
	for (i = 0; i < n; i++)
		if (!name_char(p[i]))
			return false;
	return true;
}

/*
 * Match the parcel string at @off (the length prefix) against @name.
 * Returns the total size of the string, 0 when it does not match.
 */
static unsigned int str_match_at(const unsigned char *buf, unsigned int len, unsigned int off,
				 const struct xe_name *name, int *enc)
{
	unsigned int n = name->len, total, i;

	if (off > len || len - off < 4)
		return 0;
	if (rds32(buf + off) != (int)n)
		return 0;

	total = str16_total(n);
	if (off + total <= len) {
		bool ok = true;

		for (i = 0; i < n; i++) {
			if (buf[off + 4 + 2 * i] != (unsigned char)name->p[i] || buf[off + 5 + 2 * i] != 0) {
				ok = false;
				break;
			}
		}
		if (ok && zeros(buf + off + 4 + 2 * n, total - 4 - 2 * n)) {
			*enc = 16;
			return total;
		}
	}

	total = str8_total(n);
	if (off + total <= len) {
		bool ok = true;

		for (i = 0; i < n; i++) {
			if (buf[off + 4 + i] != (unsigned char)name->p[i]) {
				ok = false;
				break;
			}
		}
		if (ok && zeros(buf + off + 4 + n, total - 4 - n)) {
			*enc = 8;
			return total;
		}
	}
	return 0;
}

/*
 * Total size of any parcel string at @off, 0 when malformed.
 * String16 is preferred when the high byte of every character is zero.
 */
static unsigned int str_len_at(const unsigned char *buf, unsigned int len, unsigned int off, int *is_null)
{
	int n;
	unsigned int un, total, i;

	*is_null = 0;
	if (off > len || len - off < 4)
		return 0;
	n = rds32(buf + off);
	if (n == -1) {
		*is_null = 1;
		return 4;
	}
	if (n < 0)
		return 0;
	un = (unsigned int)n;
	if (un > XE_MAX_NAME_LEN)
		return 0;

	total = str16_total(un);
	if (off + total <= len) {
		bool ok = true;

		for (i = 0; i < un; i++) {
			if (buf[off + 5 + 2 * i] != 0) {
				ok = false;
				break;
			}
		}
		if (ok && zeros(buf + off + 4 + 2 * un, total - 4 - 2 * un))
			return total;
	}
	total = str8_total(un);
	if (off + total <= len && zeros(buf + off + 4 + un, total - 4 - un))
		return total;
	return 0;
}

static int name_index(const struct xe_name *names, int n_names, const unsigned char *p, unsigned int len)
{
	int i;

	if (!p || !len)
		return -1;
	for (i = 0; i < n_names; i++)
		if (names[i].len == len && memcmp(names[i].p, p, len) == 0)
			return i;
	return -1;
}

/*
 * A `List<PackageInfo>` element starts with the presence int32 (1) followed by
 * the packageName string - both String16 and String8 encodings have shipped.
 * The rest of the element is opaque, which is why the caller validates the
 * element *count* before trusting any of this.
 */
static unsigned int item_start_at(const unsigned char *buf, unsigned int len, unsigned int off,
				  unsigned int *name_off, unsigned int *name_len)
{
	unsigned int n, total, i;

	if (off + 8 > len)
		return 0;
	if (rd32(buf + off) != 1)
		return 0;
	n = rd32(buf + off + 4);
	if (n == 0 || n > XE_MAX_NAME_LEN)
		return 0;

	total = str16_total(n);
	if (off + 4 + total <= len) {
		bool ok = true;

		for (i = 0; i < n; i++) {
			if (buf[off + 9 + 2 * i] != 0 || !name_char(buf[off + 8 + 2 * i])) {
				ok = false;
				break;
			}
		}
		if (ok && zeros(buf + off + 8 + 2 * n, total - 4 - 2 * n)) {
			*name_off = off + 8;
			*name_len = n;
			return 4 + total;
		}
	}

	total = str8_total(n);
	if (off + 4 + total <= len && parcel_name_ok(buf + off + 8, n) &&
	    zeros(buf + off + 8 + n, total - 4 - n)) {
		*name_off = off + 8;
		*name_len = n;
		return 4 + total;
	}
	return 0;
}

/* [int32 no-exception][int32 count][count elements] */
static unsigned int pkg_list_scan(const unsigned char *buf, unsigned int len, struct xe_item *items,
				  unsigned int max_items, unsigned int *count_out)
{
	unsigned int n, off, count = 0;

	if (len < 8 || rd32(buf) != 0)
		return 0;
	n = rd32(buf + 4);
	if (n == 0 || n > max_items || n > len / 12)
		return 0;

	for (off = 8; off + 8 <= len && count < max_items;) {
		unsigned int name_off = 0, name_len = 0, total;

		total = item_start_at(buf, len, off, &name_off, &name_len);
		if (!total) {
			off += 4;
			continue;
		}
		items[count].off = off;
		items[count].name_off = name_off;
		items[count].name_len = name_len;
		count++;
		off += total;
	}
	*count_out = count;
	if (count != n || items[0].off != 8)
		return 0;
	return count;
}

/* returns the new length, or 0 when nothing was removed */
static unsigned int rebuild_pkg_list(unsigned char *buf, unsigned int len, const struct xe_name *names,
				     int n_names)
{
	struct xe_item *items = xe_items;
	unsigned int count = 0, n, kept = 0, wr, i;

	n = pkg_list_scan(buf, len, items, XE_MAX_ITEMS, &count);
	if (n == 0)
		return 0;

	for (i = 0; i < n; i++)
		items[i].end = (i + 1 < n) ? items[i + 1].off : len;

	for (i = 0; i < n; i++) {
		if (name_index(names, n_names, buf + items[i].name_off, items[i].name_len) >= 0)
			continue;
		kept++;
	}
	if (kept == n)
		return 0; /* nothing hidden */

	wr = 8;
	for (i = 0; i < n; i++) {
		unsigned int size;

		if (name_index(names, n_names, buf + items[i].name_off, items[i].name_len) >= 0)
			continue;
		size = items[i].end - items[i].off;
		if (wr != items[i].off)
			memmove(buf + wr, buf + items[i].off, size);
		wr += size;
	}
	wr32(buf + 4, kept);
	return wr;
}

/* `String[]` reply: [int32 no-exception][int32 count][strings...] */
static unsigned int rebuild_string_array(unsigned char *buf, unsigned int len, const struct xe_name *names,
					 int n_names)
{
	struct xe_item *items = xe_items;
	int *hidden = xe_hidden;
	unsigned int n, off = 8, count, kept = 0, wr, i;

	if (len < 8 || rd32(buf) != 0)
		return 0;
	n = rd32(buf + 4);
	if (n == 0 || n > XE_MAX_ITEMS)
		return 0;

	for (count = 0; count < n; count++) {
		int is_null = 0, enc = 0;
		unsigned int total = str_len_at(buf, len, off, &is_null);

		if (!total)
			return 0;
		items[count].off = off;
		hidden[count] = -1;
		if (!is_null) {
			for (i = 0; i < (unsigned int)n_names; i++) {
				if (str_match_at(buf, len, off, &names[i], &enc) == total) {
					hidden[count] = (int)i;
					break;
				}
			}
		}
		off += total;
	}
	if (off != len)
		return 0; /* trailing bytes: not the shape we expect */

	for (i = 0; i < n; i++)
		if (hidden[i] < 0)
			kept++;
	if (kept == n)
		return 0;

	wr = 8;
	for (i = 0; i < n; i++) {
		unsigned int size = (i + 1 < n ? items[i + 1].off : len) - items[i].off;

		if (hidden[i] >= 0)
			continue;
		if (wr != items[i].off)
			memmove(buf + wr, buf + items[i].off, size);
		wr += size;
	}
	wr32(buf + 4, kept);
	return wr;
}

/* length-preserving rename; at least one name must have been replaced */
static bool substitute(unsigned char *buf, unsigned int len, const struct xe_name *names, int n_names)
{
	char fake[XE_MAX_NAME_LEN + 1];
	bool changed = false;
	unsigned int off;
	int i;

	for (i = 0; i < n_names; i++) {
		unsigned int n = names[i].len, k;

		if (n == 0 || n > XE_MAX_NAME_LEN)
			continue;
		for (k = 0; k < n; k++)
			fake[k] = names[i].p[k] == '.' ? '.' : 'q';
		fake[n] = 0;
		if (memcmp(fake, names[i].p, n) == 0)
			fake[0] = names[i].p[0] == 'z' ? 'y' : 'z';

		for (off = 0; off + 4 <= len; off++) {
			int enc = 0;
			unsigned int total, k2;

			if (rds32(buf + off) != (int)n)
				continue;
			total = str_match_at(buf, len, off, &names[i], &enc);
			if (!total)
				continue;
			for (k2 = 0; k2 < n; k2++) {
				if (enc == 16) {
					buf[off + 4 + 2 * k2] = (unsigned char)fake[k2];
					buf[off + 5 + 2 * k2] = 0;
				} else {
					buf[off + 4 + k2] = (unsigned char)fake[k2];
				}
			}
			changed = true;
			off += total - 1;
		}
	}
	return changed;
}

/*
 * Filter one parcel.
 *
 * @buf/@len : mutable private copy
 * @names    : hidden package names visible to this caller
 * @mode     : XE_PARCEL_* describing what happened
 *
 * Returns the new parcel length (<= len), or 0 when the parcel must not be
 * modified and the caller has to send the original bytes.
 */
unsigned int xe_parcel_filter(unsigned char *buf, unsigned int len, const char *const *names, int n_names,
			      unsigned int *mode)
{
	struct xe_name list[XE_MAX_NAMES];
	unsigned int new_len;
	int i;

	*mode = XE_PARCEL_NONE;
	if (!buf || len < 8 || n_names <= 0)
		return 0;
	if (n_names > XE_MAX_NAMES)
		n_names = XE_MAX_NAMES;
	for (i = 0; i < n_names; i++) {
		list[i].p = names[i];
		list[i].len = names[i] ? (unsigned int)strlen(names[i]) : 0;
		if (list[i].len > XE_MAX_NAME_LEN)
			return 0;
	}

	new_len = rebuild_pkg_list(buf, len, list, n_names);
	if (new_len && new_len <= len) {
		*mode = XE_PARCEL_REBUILT;
		return new_len;
	}
	new_len = rebuild_string_array(buf, len, list, n_names);
	if (new_len && new_len <= len) {
		*mode = XE_PARCEL_REBUILT;
		return new_len;
	}
	if (substitute(buf, len, list, n_names)) {
		*mode = XE_PARCEL_REWRITTEN;
		return len;
	}
	return 0;
}
