/* SPDX-License-Identifier: GPL-3.0-only   */
#define _GNU_SOURCE
/*
 * Host tests for the two pure translation units: xechide_match.c and
 * xechide_parcel.c. They are compiled from the same sources the module uses
 * (see tests/host/Makefile), so a green run here means the matching and parcel
 * code is at least internally consistent. The kernel glue is not covered.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xechide.h"

static int failures;
static int checks;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		checks++;                                                                                              \
		if (!(cond)) {                                                                                         \
			failures++;                                                                                    \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                        \
		}                                                                                                      \
	} while (0)

/* --------------------------------------------------------------- builders --- */


/* memmem is GNU-only and absent from the MinGW libc CI/zig use; keep the test
 * portable. */
static int contains(const unsigned char *buf, unsigned int len, const char *needle, unsigned int n)
{
	unsigned int i, k;

	if (n == 0 || len < n)
		return 0;
	/* String8 */
	for (i = 0; i + n <= len; i++)
		if (memcmp(buf + i, needle, n) == 0)
			return 1;
	/* String16 */
	for (i = 0; i + 2 * n <= len; i++) {
		for (k = 0; k < n; k++)
			if (buf[i + 2 * k] != (unsigned char)needle[k] || buf[i + 2 * k + 1] != 0)
				break;
		if (k == n)
			return 1;
	}
	return 0;
}

static unsigned int put_u32(unsigned char *b, unsigned int off, unsigned int v)
{
	memcpy(b + off, &v, 4);
	return off + 4;
}

static unsigned int put_str16(unsigned char *b, unsigned int off, const char *s)
{
	unsigned int n = (unsigned int)strlen(s), i, total = ((4u + 2u * n + 2u) + 3u) & ~3u;

	memset(b + off, 0, total);
	memcpy(b + off, &n, 4);
	for (i = 0; i < n; i++)
		b[off + 4 + 2 * i] = (unsigned char)s[i];
	return off + total;
}

static unsigned int put_str8(unsigned char *b, unsigned int off, const char *s)
{
	unsigned int n = (unsigned int)strlen(s), i, total = ((4u + n + 1u) + 3u) & ~3u;

	memset(b + off, 0, total);
	memcpy(b + off, &n, 4);
	for (i = 0; i < n; i++)
		b[off + 4 + i] = (unsigned char)s[i];
	return off + total;
}

/* one List<PackageInfo> element: presence int32 + packageName + opaque body */
static unsigned int put_pkg_item(unsigned char *b, unsigned int off, const char *pkg, int use_str8)
{
	off = put_u32(b, off, 1);
	off = use_str8 ? put_str8(b, off, pkg) : put_str16(b, off, pkg);
	off = put_u32(b, off, 0x11223344u); /* fake body: must not look like an item */
	off = put_u32(b, off, 5);
	off = put_u32(b, off, 0x00010203u);
	off = put_u32(b, off, 0);
	return off;
}

/* ---------------------------------------------------------------- matching --- */

static struct xechide_rule hidden_rule(void)
{
	struct xechide_rule r;

	memset(&r, 0, sizeof(r));
	r.target_uid = XECHIDE_UID_ALL;
	r.hidden_uid = 10123;
	r.flags = XECHIDE_RF_ALL;
	strcpy((char *)r.pkg, "com.example.hidden");
	return r;
}

static void test_match(void)
{
	struct xechide_rule r = hidden_rule();
	static const char *const yes[] = {
		"/data/app/~~aB12==/com.example.hidden-Xy_9==/base.apk",
		"/data/app/~~aB12==/com.example.hidden-Xy_9==/",
		"/data/app/com.example.hidden-1/base.apk",
		"/data/data/com.example.hidden",
		"/data/data/com.example.hidden/files/x",
		"/data/user/0/com.example.hidden",
		"/data/user_de/0/com.example.hidden",
		"/data/media/0/Android/data/com.example.hidden",
		"/data/media/0/Android/data/com.example.hidden/files",
		"/storage/emulated/0/Android/data/com.example.hidden",
		"/sdcard/Android/data/com.example.hidden",
		"/data/system/packages.list",
		"/data/system/packages.xml",
		"/data/system/appops.xml",
		"/data/system/notification_policy.xml",
		"/data/system/usagestats/0/daily/abc.bin",
		"/data/system/job/jobs.xml",
		"/data/system/shortcut_service/shortcuts.xml",
		"/data/system/users/0/package-restrictions.xml",
	};
	static const char *const no[] = {
		"/data/app/~~aB12==/com.example.hidden2-Xy/base.apk",
		"/data/app/~~aB12==/com.example.hidde-Xy/base.apk",
		"/data/data/com.example.hidden2",
		"/data/data/com.example.hidden2/files",
		"/data/user/0/com.example.hidden2",
		"/data/user/10/com.example.hidden2",
		"/data/media/0/Android/data/com.example.hidden2",
		"/storage/emulated/0/Android/data/com.example.hidden2",
		"/system/app/com.example.hidden/base.apk",
		"/data/system/packages.list.bak",
		"/data/system/jobs.xml",
		"/data/system/users/0/other.xml",
		"/data/system/usagestats2/0/x",
		"/proc/1234",
		"",
	};
	unsigned int i;

	for (i = 0; i < sizeof(yes) / sizeof(yes[0]); i++)
		CHECK(xe_match_path(&r, yes[i]) == 1);
	for (i = 0; i < sizeof(no) / sizeof(no[0]); i++)
		CHECK(xe_match_path(&r, no[i]) == 0);

	CHECK(xe_match_dirent(&r, XE_DIR_APP, "com.example.hidden-AbCd==", -1) == 1);
	CHECK(xe_match_dirent(&r, XE_DIR_APP, "com.example.hidden", 18) == 0);
	CHECK(xe_match_dirent(&r, XE_DIR_APP, "com.example.hidden2-AbCd==", -1) == 0);
	CHECK(xe_match_dirent(&r, XE_DIR_DATA, "com.example.hidden", 18) == 1);
	CHECK(xe_match_dirent(&r, XE_DIR_DATA, "com.example.hidden2", 19) == 0);
	CHECK(xe_match_dirent(&r, XE_DIR_MEDIA, "com.example.hidden", 18) == 1);
	CHECK(xe_match_dirent(&r, XE_DIR_PROC, "10123", 5) == 0);

	/* the rule flags gate every layer */
	r.flags = XECHIDE_RF_BINDER;
	CHECK(xe_match_path(&r, "/data/data/com.example.hidden") == 0);
	CHECK(xe_match_dirent(&r, XE_DIR_DATA, "com.example.hidden", 18) == 0);
}

/* ---------------------------------------------------------------- parcels --- */

static const char *const k_names[] = { "com.example.hidden" };
#define HIDDEN "com.example.hidden"

static void test_list_removal_str16(void)
{
	unsigned char buf[1024];
	unsigned int len = 0, mode = 0, new_len, count;

	len = put_u32(buf, len, 0); /* no exception */
	len = put_u32(buf, len, 3); /* three packages */
	len = put_pkg_item(buf, len, "com.example.visible", 0);
	len = put_pkg_item(buf, len, HIDDEN, 0);
	len = put_pkg_item(buf, len, "com.android.other", 0);

	new_len = xe_parcel_filter(buf, len, k_names, 1, &mode);
	CHECK(mode == XE_PARCEL_REBUILT);
	CHECK(new_len && new_len < len);
	if (new_len) {
		memcpy(&count, buf + 4, 4);
		CHECK(count == 2);
		CHECK(!contains(buf, new_len, HIDDEN, 18));
		CHECK(contains(buf, new_len, "com.example.visible", 19));
		CHECK(contains(buf, new_len, "com.android.other", 17));
	}
}

static void test_list_removal_str8(void)
{
	unsigned char buf[1024];
	unsigned int len = 0, mode = 0, new_len, count;

	len = put_u32(buf, len, 0);
	len = put_u32(buf, len, 2);
	len = put_pkg_item(buf, len, HIDDEN, 1);
	len = put_pkg_item(buf, len, "com.example.visible", 1);

	new_len = xe_parcel_filter(buf, len, k_names, 1, &mode);
	CHECK(mode == XE_PARCEL_REBUILT);
	CHECK(new_len && new_len < len);
	if (new_len) {
		memcpy(&count, buf + 4, 4);
		CHECK(count == 1);
		CHECK(!contains(buf, new_len, HIDDEN, 18));
	}
}

static void test_string_array(void)
{
	unsigned char buf[512];
	unsigned int len = 0, mode = 0, new_len, count;

	len = put_u32(buf, len, 0);
	len = put_u32(buf, len, 3);
	len = put_str16(buf, len, HIDDEN);
	len = put_str16(buf, len, "com.android.other");
	len = put_str16(buf, len, HIDDEN);

	new_len = xe_parcel_filter(buf, len, k_names, 1, &mode);
	CHECK(mode == XE_PARCEL_REBUILT);
	CHECK(new_len && new_len < len);
	if (new_len) {
		memcpy(&count, buf + 4, 4);
		CHECK(count == 1);
		CHECK(contains(buf, new_len, "com.android.other", 17));
	}
}

/* A parcel that does not validate must be renamed, not restructured. */
static void test_substitution_fallback(void)
{
	unsigned char buf[512], head[8];
	unsigned int len = 0, mode = 0, new_len;

	len = put_u32(buf, len, 0);
	len = put_u32(buf, len, 5); /* claims five items, carries two */
	len = put_pkg_item(buf, len, HIDDEN, 0);
	len = put_pkg_item(buf, len, "com.example.visible", 0);
	memcpy(head, buf, 8);

	new_len = xe_parcel_filter(buf, len, k_names, 1, &mode);
	CHECK(mode == XE_PARCEL_REWRITTEN);
	CHECK(new_len == len); /* layout must not change */
	CHECK(memcmp(buf, head, 8) == 0);
	CHECK(!contains(buf, len, HIDDEN, 18));
	CHECK(contains(buf, len, "com.example.visible", 19));
}

/* Requests are not lists: same in-place rename, no structural change. */
static void test_request_rename(void)
{
	unsigned char buf[512];
	unsigned int len = 0, mode = 0, new_len;

	len = put_str16(buf, len, "android.content.pm.IPackageManager");
	len = put_str16(buf, len, HIDDEN);
	len = put_u32(buf, len, 0); /* flags */
	len = put_u32(buf, len, 0); /* userId */

	new_len = xe_parcel_filter(buf, len, k_names, 1, &mode);
	CHECK(mode == XE_PARCEL_REWRITTEN);
	CHECK(new_len == len);
	CHECK(!contains(buf, len, HIDDEN, 18));
	CHECK(contains(buf, len, "android.content.pm.IPackageManager", 34));
}

/* Process names ("pkg:remote") and flattened component names ("pkg/.Main")
 * must be renamed on the package part only. */
static void test_component_rename(void)
{
	unsigned char buf[512];
	unsigned int len = 0, mode = 0, new_len;

	len = put_str16(buf, len, "com.example.hidden:remote");
	len = put_str16(buf, len, "com.example.hidden/.MainActivity");
	len = put_str16(buf, len, "com.example.hidde2:remote");
	len = put_str16(buf, len, "com.example.hidden");

	new_len = xe_parcel_filter(buf, len, k_names, 1, &mode);
	CHECK(mode == XE_PARCEL_REWRITTEN);
	CHECK(new_len == len);
	CHECK(contains(buf, len, "com.example.hidden", 18) == 0);
	CHECK(contains(buf, len, "com.example.hidde2:remote", 25) == 1);
	CHECK(contains(buf, len, ":remote", 7) == 1);
	CHECK(contains(buf, len, "/.MainActivity", 14) == 1);
	CHECK(contains(buf, len, "qqq.qqqqqqq.qqqqqq", 18) == 1);
}

static void test_negative(void)
{
	unsigned char buf[256];
	unsigned int mode = 0, len = 0;

	memset(buf, 0xaa, sizeof(buf));
	CHECK(xe_parcel_filter(buf, 4, k_names, 1, &mode) == 0);
	CHECK(mode == XE_PARCEL_NONE);

	memset(buf, 0, sizeof(buf));
	buf[0] = 1; /* nonsense header */
	CHECK(xe_parcel_filter(buf, 12, k_names, 1, &mode) == 0);
	CHECK(mode == XE_PARCEL_NONE);

	CHECK(xe_parcel_filter(NULL, 12, k_names, 1, &mode) == 0);
	CHECK(xe_parcel_filter(buf, 12, NULL, 1, &mode) == 0);

	/* a parcel without a hidden name must be left alone */
	len = put_u32(buf, len, 0);
	len = put_u32(buf, len, 1);
	len = put_pkg_item(buf, len, "com.example.visible", 0);
	mode = 0;
	CHECK(xe_parcel_filter(buf, len, k_names, 1, &mode) == 0);
	CHECK(mode == XE_PARCEL_NONE);
}

int main(void)
{
	printf("xechide host tests\n");
	printf(" match:\n");
	test_match();
	printf(" parcels:\n");
	test_list_removal_str16();
	test_list_removal_str8();
	test_string_array();
	test_substitution_fallback();
	test_request_rename();
	test_component_rename();
	test_negative();
	printf("%d checks, %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}
