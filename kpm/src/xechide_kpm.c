/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * XEChide - native KernelPatch Module (KPM) form.
 *
 * This is a real KPM: ET_REL AArch64, built with the public KernelPatch KPM
 * headers and loaded by APatch/KernelPatch's KPM engine (also usable on any
 * KPM engine that provides the KernelPatch runtime). It is the kernel-form
 * enforcement without a custom kernel image:
 *
 *   - binder: hook binder_transaction / binder_thread_read, rewrite the same
 *     parcels the KernelSU kernel patch handles (request names + BR_REPLY
 *     containers), reusing kernel/xechide_parcel.c verbatim;
 *   - files: hook openat/newfstatat/faccessat/readlinkat/statx/openat2 and
 *     getdents64 for the current uid, hiding configured paths and directory
 *     entries at syscall entry/exit;
 *   - config: /data/adb/xechide/config (the same 8976-byte struct), read by
 *     the root KPM context; `ctl0 reload` re-reads it atomically.
 *
 * Everything fails open: a missing symbol, a failed hook or an unknown parcel
 * leaves the original syscall/parcel untouched. No userspace process is
 * involved in enforcement.
 */
#include <compiler.h>
#include <kpmodule.h>
#include <kallsyms.h>
#include <kputils.h>
#include <common.h>
#include <log.h>
#include <hook.h>
#include <syscall.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <asm/current.h>
#include <uapi/asm-generic/unistd.h>

#include "xechide.h"

/* KPM's kernel headers are a trimmed set; the kernel's struct dirent layout is
 * stable and only d_reclen/d_name are read here. */
struct linux_dirent64 {
	uint64_t d_ino;
	int64_t d_off;
	unsigned short d_reclen;
	unsigned char d_type;
	char d_name[];
};

KPM_NAME("xechide");
KPM_VERSION("1.0.0");
KPM_LICENSE("GPL v2");
KPM_AUTHOR("XEChide");
KPM_DESCRIPTION("XEChide kernel hiding: binder parcel filter + path/dirent hiding (KPM)");

#define XE_CFG_PATH "/data/adb/xechide/config"
/* APatch's KPM loader/control runs in the APatch manager app's uid. The
 * manager app cannot traverse /data/adb, so the root helper also mirrors the
 * packed config into APatch's private data files (0600, APatch uid). The KPM
 * tries the root path first (boot/root contexts) and this path second. */
#define XE_CFG_APATCH "/data/data/me.bmax.apatch/files/xechide.cfg"
#define XE_PARCEL_MAX (256u * 1024u)
#define XE_HEAD_SCAN 1024u
#define XE_MARKS 64
#define XE_NAMES_MAX 16
#define XE_PATH_MAX 512

#ifndef O_RDONLY
#define O_RDONLY 0
#endif

/* KPM's asm-generic header only exposes the kernel-internal 3264 names for a
 * few syscalls; these are the stable arm64 UAPI numbers used by the hooks. */
#ifndef __NR_newfstatat
#define __NR_newfstatat 79
#endif
#ifndef __NR_statx
#define __NR_statx 291
#endif
#ifndef __NR_openat2
#define __NR_openat2 437
#endif
#ifndef __NR_faccessat2
#define __NR_faccessat2 439
#endif

/* UAPI binder transaction layout, deliberately local so no binder-internal
 * header is needed. binder_size_t / binder_uintptr_t are __u64 on every ABI. */
struct xe_btd {
	union {
		uint32_t handle;
		uint64_t ptr;
	} target;
	uint64_t cookie;
	uint32_t code;
	uint32_t flags;
	int32_t sender_pid;
	uint32_t sender_euid;
	uint64_t data_size;
	uint64_t offsets_size;
	union {
		struct {
			uint64_t buffer;
			uint64_t offsets;
		} ptr;
		uint8_t buf[8];
	} data;
};

#define XE_BR_ERROR 0x80047200u
#define XE_BR_OK 0x00007201u
#define XE_BR_TRANSACTION_SEC_CTX 0x80487202u
#define XE_BR_TRANSACTION 0x80407202u
#define XE_BR_REPLY 0x80407203u
#define XE_BR_ACQUIRE_RESULT 0x80047204u
#define XE_BR_DEAD_REPLY 0x00007205u
#define XE_BR_TRANSACTION_COMPLETE 0x00007206u
#define XE_BR_INCREFS 0x80107207u
#define XE_BR_ACQUIRE 0x80107208u
#define XE_BR_RELEASE 0x80107209u
#define XE_BR_DECREFS 0x8010720au
#define XE_BR_ATTEMPT_ACQUIRE 0x8018720bu
#define XE_BR_NOOP 0x0000720cu
#define XE_BR_SPAWN_LOOPER 0x0000720du
#define XE_BR_FINISHED 0x0000720eu
#define XE_BR_DEAD_BINDER 0x8008720fu
#define XE_BR_CLEAR_DEATH_NOTIFICATION_DONE 0x80087210u
#define XE_BR_FAILED_REPLY 0x00007211u
#define XE_BR_FROZEN_REPLY 0x00007212u
#define XE_BR_ONEWAY_SPAM_SUSPECT 0x00007213u
#define XE_BR_TRANSACTION_PENDING_FROZEN 0x00007214u
#define XE_BR_FROZEN_BINDER 0x80107215u
#define XE_BR_CLEAR_FREEZE_NOTIFICATION_DONE 0x80087216u

static struct xechide_config g_cfg_a;
static struct xechide_config g_cfg_b;
static struct xechide_config *g_cfg = &g_cfg_a;
static volatile int g_loaded;

static unsigned char g_binder_buf[XE_PARCEL_MAX];
static unsigned char g_read_buf[XE_PARCEL_MAX];
static unsigned char g_parcel_buf[XE_PARCEL_MAX];
static unsigned char g_dir_buf[64 * 1024];
static int g_binder_lock;
static int g_read_lock;
static int g_dir_lock;
static int g_mark_lock;
static int g_filter_lock;

struct xe_mark {
	int32_t pid;
	uint32_t uid;
};
static struct xe_mark g_marks[XE_MARKS];

/* Resolved at init; NULL disables the dependent feature (fail-open). */
static struct file *(*g_filp_open)(const char *, int, umode_t);
static ssize_t (*g_kernel_read)(struct file *, void *, size_t, loff_t *);
static int (*g_filp_close)(struct file *, void *);
static long (*g_cfu_nofault)(void *, const void __user *, unsigned long);
static long (*g_ctu_nofault)(void __user *, const void *, unsigned long);

static void *g_binder_transaction;
static void *g_binder_thread_read;
static int g_binder_ok;
static int g_syscall_ok[8];

static inline struct xechide_config *cfg_get(void)
{
	return __atomic_load_n(&g_cfg, __ATOMIC_ACQUIRE);
}

static inline void cfg_publish(struct xechide_config *cfg)
{
	__atomic_store_n(&g_cfg, cfg, __ATOMIC_RELEASE);
}

static int atomic_try(int *lock)
{
	return !__atomic_test_and_set(lock, __ATOMIC_ACQUIRE);
}

static void atomic_drop(int *lock)
{
	__atomic_clear(lock, __ATOMIC_RELEASE);
}

static int cfg_rule_pkg_ok(const struct xechide_rule *r)
{
	const char *p = (const char *)r->pkg;
	int i;

	for (i = 0; i < XECHIDE_PKG_LEN; i++) {
		unsigned char c = (unsigned char)p[i];

		if (!c)
			return i > 0;
		if (c >= 'a' && c <= 'z')
			continue;
		if (c >= 'A' && c <= 'Z')
			continue;
		if (c >= '0' && c <= '9')
			continue;
		if (c == '.' || c == '_')
			continue;
		return 0;
	}
	return 0;
}

static int cfg_valid(const struct xechide_config *cfg)
{
	unsigned int i;

	if (cfg->magic != XECHIDE_ABI_MAGIC || cfg->version != XECHIDE_ABI_VERSION)
		return 0;
	if (cfg->n_rules > XECHIDE_MAX_RULES)
		return 0;
	for (i = 0; i < cfg->n_rules; i++) {
		const struct xechide_rule *r = &cfg->rules[i];

		if (!cfg_rule_pkg_ok(r))
			return 0;
		if (r->hidden_uid == 0 || r->hidden_uid == 1000)
			return 0;
		if (r->target_uid == 0 || r->target_uid == 1000)
			return 0;
		if (!(r->flags & XECHIDE_RF_ALL))
			return 0;
	}
	return 1;
}

static int read_exact(struct file *f, void *buf, size_t len)
{
	loff_t pos = 0;
	size_t got = 0;

	while (got < len) {
		ssize_t n = g_kernel_read(f, (char *)buf + got, len - got, &pos);

		if (n <= 0)
			break;
		got += (size_t)n;
	}
	return got == len ? 0 : -1;
}

static int read_config_file(const char *path, struct xechide_config *out)
{
	struct file *f;

	if (!g_filp_open || !g_kernel_read || !g_filp_close)
		return -1;
	f = g_filp_open(path, O_RDONLY, 0);
	if (!f || ((unsigned long)f >= (unsigned long)-4095))
		return -1;
	memset(out, 0, sizeof(*out));
	if (read_exact(f, out, sizeof(*out)) < 0) {
		g_filp_close(f, 0);
		return -1;
	}
	g_filp_close(f, 0);
	return 0;
}

static int load_config(void)
{
	struct xechide_config *active = cfg_get();
	struct xechide_config *next = active == &g_cfg_a ? &g_cfg_b : &g_cfg_a;

	if (read_config_file(XE_CFG_PATH, next) < 0 &&
	    read_config_file(XE_CFG_APATCH, next) < 0)
		return -1;
	if (!cfg_valid(next))
		return -1;
	cfg_publish(next);
	__atomic_store_n(&g_loaded, 1, __ATOMIC_RELEASE);
	return 0;
}

static uint32_t current_uid32(void)
{
	return (uint32_t)current_uid();
}

static int collect_names(uint32_t uid, uint32_t need_flags, const char **names, int max)
{
	struct xechide_config *cfg = cfg_get();
	int n = 0;
	unsigned int i;

	if (!cfg)
		return 0;
	for (i = 0; i < cfg->n_rules && n < max; i++) {
		const struct xechide_rule *r = &cfg->rules[i];

		if (!(r->flags & need_flags))
			continue;
		if (r->target_uid != XECHIDE_UID_ALL && r->target_uid != uid)
			continue;
		if (!cfg_rule_pkg_ok(r))
			continue;
		names[n++] = (const char *)r->pkg;
	}
	return n;
}

static int name_is_hidden(const char *name, const char *const *names, int n)
{
	int i;

	for (i = 0; i < n; i++)
		if (!strcmp(name, names[i]))
			return 1;
	return 0;
}

static int self_hidden_path(const char *path)
{
	static const char *const prefixes[] = {
		"/data/adb/xechide",
		"/data/adb/kpm/xechide",
		"/data/adb/ap/kpm/xechide",
		"/data/adb/kp/kpm/xechide",
		"/data/adb/modules/xechide",
	};
	unsigned int i;

	for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		size_t n = strlen(prefixes[i]);

		if (!strncmp(path, prefixes[i], n) && (path[n] == 0 || path[n] == '/'))
			return 1;
	}
	return 0;
}

static int path_hidden(uint32_t uid, const char *path)
{
	struct xechide_config *cfg = cfg_get();
	unsigned int i;

	if (!cfg || !path || path[0] != '/')
		return 0;
	if (uid == 0 || uid == 1000)
		return 0;
	if ((cfg->flags & XECHIDE_CF_HIDE_SELF) && self_hidden_path(path))
		return 1;
	for (i = 0; i < cfg->n_rules; i++) {
		const struct xechide_rule *r = &cfg->rules[i];

		if (!(r->flags & XECHIDE_RF_PATHS))
			continue;
		if (r->target_uid != XECHIDE_UID_ALL && r->target_uid != uid)
			continue;
		if (xe_match_path(r, path))
			return 1;
	}
	return 0;
}

/* ------------------------------------------------------------- syscalls --- */

static int copy_path(const char __user *u, char *out)
{
	long n;

	if (!u)
		return -1;
	n = compat_strncpy_from_user(out, u, XE_PATH_MAX - 1);
	if (n <= 0)
		return -1;
	out[XE_PATH_MAX - 1] = 0;
	return 0;
}

static void before_path_syscall(hook_fargs4_t *args, void *udata)
{
	const char __user *u = (const char __user *)syscall_argn(args, 1);
	char path[XE_PATH_MAX];

	(void)udata;
	if (copy_path(u, path) < 0)
		return;
	if (path_hidden(current_uid32(), path)) {
		args->skip_origin = 1;
		args->ret = (uint64_t)(long)-ENOENT;
	}
}

static void after_getdents64(hook_fargs4_t *args, void *udata)
{
	unsigned char *user = (unsigned char *)syscall_argn(args, 1);
	long ret = (long)args->ret;
	const char *names[XE_NAMES_MAX];
	int n_names, used, off, wr;
	uint32_t uid = current_uid32();

	(void)udata;
	if (ret <= 0 || !user || ret > (long)sizeof(g_dir_buf))
		return;
	if (uid == 0 || uid == 1000)
		return;
	n_names = collect_names(uid, XECHIDE_RF_PATHS, names, XE_NAMES_MAX);
	if (n_names <= 0)
		return;
	if (!g_cfu_nofault || !g_ctu_nofault)
		return;
	if (!atomic_try(&g_dir_lock))
		return;
	used = (int)ret;
	if (g_cfu_nofault(g_dir_buf, user, (unsigned long)used)) {
		atomic_drop(&g_dir_lock);
		return;
	}
	wr = 0;
	off = 0;
	while (off + 19 <= used) {
		struct linux_dirent64 *d = (struct linux_dirent64 *)(g_dir_buf + off);
		unsigned int reclen = d->d_reclen;

		if (reclen < 19 || off + (int)reclen > used)
			break;
		if (d->d_name[0] && !name_is_hidden(d->d_name, names, n_names)) {
			if (wr != off)
				memmove(g_dir_buf + wr, g_dir_buf + off, reclen);
			wr += (int)reclen;
		}
		off += (int)reclen;
	}
	if (wr != used && wr >= 0) {
		if (!g_ctu_nofault(user, g_dir_buf, (unsigned long)wr))
			args->ret = (uint64_t)(long)wr;
	}
	atomic_drop(&g_dir_lock);
}

/* ---------------------------------------------------------------- binder --- */

static int32_t current_tid(void)
{
	if (task_struct_offset.pid_offset <= 0)
		return -1;
	return *(int32_t *)((char *)current + task_struct_offset.pid_offset);
}

static void mark_add(int32_t pid, uint32_t uid)
{
	int i;

	if (pid < 0 || !atomic_try(&g_mark_lock))
		return;
	for (i = 0; i < XE_MARKS; i++) {
		if (g_marks[i].pid == pid) {
			g_marks[i].uid = uid;
			atomic_drop(&g_mark_lock);
			return;
		}
	}
	for (i = 0; i < XE_MARKS; i++) {
		if (g_marks[i].pid == 0) {
			g_marks[i].pid = pid;
			g_marks[i].uid = uid;
			break;
		}
	}
	atomic_drop(&g_mark_lock);
}

static int mark_take(int32_t pid, uint32_t *uid)
{
	int i, found = 0;

	if (pid < 0 || !atomic_try(&g_mark_lock))
		return 0;
	for (i = 0; i < XE_MARKS; i++) {
		if (g_marks[i].pid == pid) {
			*uid = g_marks[i].uid;
			g_marks[i].pid = 0;
			g_marks[i].uid = 0;
			found = 1;
			break;
		}
	}
	atomic_drop(&g_mark_lock);
	return found;
}

static int zeros_after(const unsigned char *p, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		if (p[i])
			return 0;
	return 1;
}

static int parcel_has_token(const unsigned char *buf, unsigned int len, const char *token)
{
	unsigned int n = (unsigned int)strlen(token), off, total, i;

	if (len < 8 || !n)
		return 0;
	if (len > XE_HEAD_SCAN)
		len = XE_HEAD_SCAN;
	for (off = 0; off + 4 <= len; off++) {
		uint32_t v;

		memcpy(&v, buf + off, 4);
		if (v != n)
			continue;
		total = (4u + 2u * n + 2u + 3u) & ~3u;
		if (off + total <= len) {
			int ok = 1;

			for (i = 0; i < n; i++) {
				if (buf[off + 4 + 2 * i] != (unsigned char)token[i] ||
				    buf[off + 5 + 2 * i] != 0) {
					ok = 0;
					break;
				}
			}
			if (ok && zeros_after(buf + off + 4 + 2 * n, total - 4 - 2 * n))
				return 1;
		}
		total = (4u + n + 1u + 3u) & ~3u;
		if (off + total <= len && !memcmp(buf + off + 4, token, n) &&
		    zeros_after(buf + off + 4 + n, total - 4 - n))
			return 1;
	}
	return 0;
}

static const char *const g_tokens[] = {
	"android.content.pm.IPackageManager",
	"android.app.IActivityManager",
	"android.app.IActivityTaskManager",
	"android.content.pm.ILauncherApps",
	"android.content.pm.IPackageInstaller",
	"android.content.pm.IShortcutService",
	"android.app.usage.IUsageStatsManager",
	"com.android.internal.app.IAppOpsService",
	"android.app.job.IJobScheduler",
	"android.app.INotificationManager",
	"android.view.IWindowManager",
	"android.view.accessibility.IAccessibilityManager",
};

static int parcel_names_service(const unsigned char *buf, unsigned int len)
{
	unsigned int i;

	for (i = 0; i < sizeof(g_tokens) / sizeof(g_tokens[0]); i++)
		if (parcel_has_token(buf, len, g_tokens[i]))
			return 1;
	return 0;
}

static void before_binder_transaction(hook_fargs5_t *args, void *udata)
{
	struct xe_btd *tr = (struct xe_btd *)(uintptr_t)args->arg2;
	uint64_t buffer;
	uint64_t size;
	uint32_t uid = current_uid32();
	const char *names[XE_NAMES_MAX];
	int n_names;
	unsigned int mode = 0;
	unsigned int new_len;

	(void)udata;
	if ((int)args->arg3)
		return; /* reply direction is handled on the read side */
	if (!tr || !g_binder_ok || !g_cfu_nofault || !g_ctu_nofault)
		return;
	size = tr->data_size;
	buffer = tr->data.ptr.buffer;
	if (!buffer || size < 8 || size > XE_PARCEL_MAX)
		return;
	n_names = collect_names(uid, XECHIDE_RF_BINDER, names, XE_NAMES_MAX);
	if (n_names <= 0)
		return;
	if (!atomic_try(&g_binder_lock))
		return;
	g_cfu_nofault(g_binder_buf, (const void __user *)(uintptr_t)buffer,
				  size > XE_HEAD_SCAN ? XE_HEAD_SCAN : size);
	if (!parcel_names_service(g_binder_buf, size > XE_HEAD_SCAN ? XE_HEAD_SCAN : (unsigned int)size)) {
		atomic_drop(&g_binder_lock);
		return;
	}
	if (g_cfu_nofault(g_binder_buf, (const void __user *)(uintptr_t)buffer, (unsigned long)size)) {
		atomic_drop(&g_binder_lock);
		return;
	}
	if (!atomic_try(&g_filter_lock)) {
		atomic_drop(&g_binder_lock);
		return;
	}
	new_len = xe_parcel_filter(g_binder_buf, (unsigned int)size, names, n_names, &mode);
	atomic_drop(&g_filter_lock);
	if (mode == XE_PARCEL_REWRITTEN && new_len == size)
		g_ctu_nofault((void __user *)(uintptr_t)buffer, g_binder_buf, (unsigned long)size);
	atomic_drop(&g_binder_lock);
	mark_add(current_tid(), uid);
}

static int filter_reply_buf(uint32_t uid, unsigned char *buf, unsigned int used)
{
	unsigned int off = 0;
	const char *names[XE_NAMES_MAX];
	int n_names = collect_names(uid, XECHIDE_RF_BINDER, names, XE_NAMES_MAX);

	if (n_names <= 0)
		return 0;
	while (off + 4 <= used) {
		uint32_t cmd, mode = 0, new_len;

		memcpy(&cmd, buf + off, 4);
		if (cmd == XE_BR_REPLY) {
			struct xe_btd tr;

			if (off + 4 + sizeof(tr) > used)
				return 0;
			memcpy(&tr, buf + off + 4, sizeof(tr));
			if (!tr.data_size || tr.data_size > XE_PARCEL_MAX || tr.offsets_size ||
			    !tr.data.ptr.buffer)
				return 0;
			if (!g_cfu_nofault(g_parcel_buf,
						       (const void __user *)(uintptr_t)tr.data.ptr.buffer,
						       (unsigned long)tr.data_size))
				return 0;
			if (!atomic_try(&g_filter_lock))
				return 0;
			new_len = xe_parcel_filter(g_parcel_buf, (unsigned int)tr.data_size, names,
						   n_names, &mode);
			atomic_drop(&g_filter_lock);
			if (!new_len || new_len > tr.data_size)
				return 0;
			if (g_ctu_nofault((void __user *)(uintptr_t)tr.data.ptr.buffer,
						    g_parcel_buf, new_len))
				return 0;
			tr.data_size = new_len;
			memcpy(buf + off + 4, &tr, sizeof(tr));
			return 1;
		}
		switch (cmd) {
		case XE_BR_OK:
		case XE_BR_DEAD_REPLY:
		case XE_BR_TRANSACTION_COMPLETE:
		case XE_BR_NOOP:
		case XE_BR_SPAWN_LOOPER:
		case XE_BR_FINISHED:
		case XE_BR_FAILED_REPLY:
		case XE_BR_FROZEN_REPLY:
		case XE_BR_ONEWAY_SPAM_SUSPECT:
		case XE_BR_TRANSACTION_PENDING_FROZEN:
			off += 4;
			break;
		case XE_BR_ERROR:
		case XE_BR_ACQUIRE_RESULT:
			off += 8;
			break;
		case XE_BR_INCREFS:
		case XE_BR_ACQUIRE:
		case XE_BR_RELEASE:
		case XE_BR_DECREFS:
			off += 4 + 16;
			break;
		case XE_BR_ATTEMPT_ACQUIRE:
			off += 4 + 24;
			break;
		case XE_BR_DEAD_BINDER:
		case XE_BR_CLEAR_DEATH_NOTIFICATION_DONE:
		case XE_BR_CLEAR_FREEZE_NOTIFICATION_DONE:
			off += 4 + 8;
			break;
		case XE_BR_FROZEN_BINDER:
			off += 4 + 16;
			break;
		case XE_BR_TRANSACTION_SEC_CTX:
			off += 4 + 72;
			break;
		case XE_BR_TRANSACTION:
			off += 4 + sizeof(struct xe_btd);
			break;
		default:
			return 0;
		}
	}
	return 0;
}

static void after_binder_thread_read(hook_fargs6_t *args, void *udata)
{
	const void __user *userbuf = (const void __user *)(uintptr_t)args->arg2;
	/* binder_ioctl passes &bwr.read_consumed (a kernel stack address), not a
	 * user pointer: binder_thread_read() is only called from that ioctl with
	 * the kernel copy of struct binder_write_read. Read it directly. */
	const uint64_t *consumed_k = (const uint64_t *)(uintptr_t)args->arg4;
	uint64_t used = 0;
	uint32_t uid = 0;

	(void)udata;
	if ((long)args->ret < 0 || !userbuf || !consumed_k || !g_binder_ok)
		return;
	if (!g_ctu_nofault)
		return;
	used = *(volatile const uint64_t *)consumed_k;
	if (!used || used > XE_PARCEL_MAX)
		return;
	if (!mark_take(current_tid(), &uid))
		return;
	if (uid == 0 || uid == 1000)
		return;
	if (!atomic_try(&g_read_lock))
		return;
	if (!g_cfu_nofault(g_read_buf, userbuf, (unsigned long)used)) {
		if (filter_reply_buf(uid, g_read_buf, (unsigned int)used))
			g_ctu_nofault((void __user *)(uintptr_t)userbuf, g_read_buf,
						(unsigned long)used);
	}
	atomic_drop(&g_read_lock);
}

/* ----------------------------------------------------------------- init --- */

static void resolve_symbols(void)
{
	g_filp_open = (void *)kallsyms_lookup_name("filp_open");
	g_kernel_read = (void *)kallsyms_lookup_name("kernel_read");
	g_filp_close = (void *)kallsyms_lookup_name("filp_close");
	g_cfu_nofault = (void *)kallsyms_lookup_name("copy_from_user_nofault");
	g_ctu_nofault = (void *)kallsyms_lookup_name("copy_to_user_nofault");
	g_binder_transaction = (void *)kallsyms_lookup_name("binder_transaction");
	g_binder_thread_read = (void *)kallsyms_lookup_name("binder_thread_read");
}

static int install_hooks(void)
{
	if (g_binder_transaction && g_binder_thread_read && g_cfu_nofault &&
	    g_ctu_nofault) {
		hook_err_t e1 = hook_wrap5(g_binder_transaction, before_binder_transaction, 0, 0);
		hook_err_t e2 = hook_wrap6(g_binder_thread_read, 0, after_binder_thread_read, 0);

		if (e1 == HOOK_NO_ERR && e2 == HOOK_NO_ERR) {
			g_binder_ok = 1;
		} else {
			if (e1 == HOOK_NO_ERR)
				hook_unwrap(g_binder_transaction, before_binder_transaction, 0);
			if (e2 == HOOK_NO_ERR)
				hook_unwrap(g_binder_thread_read, 0, after_binder_thread_read);
		}
	}

	g_syscall_ok[0] = hook_syscalln(__NR_openat, 4, before_path_syscall, 0, 0) == HOOK_NO_ERR;
	g_syscall_ok[1] = hook_syscalln(__NR_newfstatat, 4, before_path_syscall, 0, 0) == HOOK_NO_ERR;
	g_syscall_ok[2] = hook_syscalln(__NR_faccessat, 3, before_path_syscall, 0, 0) == HOOK_NO_ERR;
	g_syscall_ok[3] = hook_syscalln(__NR_readlinkat, 4, before_path_syscall, 0, 0) == HOOK_NO_ERR;
	g_syscall_ok[4] = hook_syscalln(__NR_statx, 5, before_path_syscall, 0, 0) == HOOK_NO_ERR;
	g_syscall_ok[5] = hook_syscalln(__NR_openat2, 4, before_path_syscall, 0, 0) == HOOK_NO_ERR;
	g_syscall_ok[6] = hook_syscalln(__NR_getdents64, 3, 0, after_getdents64, 0) == HOOK_NO_ERR;
	g_syscall_ok[7] = hook_syscalln(__NR_faccessat2, 4, before_path_syscall, 0, 0) == HOOK_NO_ERR;
	pr_info("xechide-kpm: binder=%d syscalls=%d,%d,%d,%d,%d,%d,%d,%d\n", g_binder_ok,
		g_syscall_ok[0], g_syscall_ok[1], g_syscall_ok[2], g_syscall_ok[3],
		g_syscall_ok[4], g_syscall_ok[5], g_syscall_ok[6], g_syscall_ok[7]);
	return 0;
}

static void remove_hooks(void)
{
	if (g_binder_ok) {
		hook_unwrap(g_binder_transaction, before_binder_transaction, 0);
		hook_unwrap(g_binder_thread_read, 0, after_binder_thread_read);
		g_binder_ok = 0;
	}
	if (g_syscall_ok[0])
		unhook_syscalln(__NR_openat, before_path_syscall, 0);
	if (g_syscall_ok[1])
		unhook_syscalln(__NR_newfstatat, before_path_syscall, 0);
	if (g_syscall_ok[2])
		unhook_syscalln(__NR_faccessat, before_path_syscall, 0);
	if (g_syscall_ok[3])
		unhook_syscalln(__NR_readlinkat, before_path_syscall, 0);
	if (g_syscall_ok[4])
		unhook_syscalln(__NR_statx, before_path_syscall, 0);
	if (g_syscall_ok[5])
		unhook_syscalln(__NR_openat2, before_path_syscall, 0);
	/* getdents64 after-only hook: use the paired before pointer (NULL). */
	if (g_syscall_ok[6])
		unhook_syscalln(__NR_getdents64, 0, after_getdents64);
	if (g_syscall_ok[7])
		unhook_syscalln(__NR_faccessat2, before_path_syscall, 0);
}

static long xechide_ctl(const char *args, char __user *out_msg, int outlen)
{
	char msg[256];
	int n;

	if (!args)
		args = "status";
	if (!strcmp(args, "reload")) {
		if (load_config() == 0) {
			n = snprintf(msg, sizeof(msg), "reload=ok rules=%u\n",
				     cfg_get()->n_rules);
		} else {
			n = snprintf(msg, sizeof(msg), "reload=failed\n");
		}
	} else if (!strcmp(args, "status")) {
		struct xechide_config *cfg = cfg_get();

		n = snprintf(msg, sizeof(msg),
			     "loaded=%d rules=%u binder=%d path=%d getdents=%d\n",
			     __atomic_load_n(&g_loaded, __ATOMIC_ACQUIRE) ? 1 : 0,
			     cfg ? cfg->n_rules : 0, g_binder_ok,
			     g_syscall_ok[0] || g_syscall_ok[1] || g_syscall_ok[2] ||
				     g_syscall_ok[3] || g_syscall_ok[4] || g_syscall_ok[5],
			     g_syscall_ok[6]);
	} else if (!strcmp(args, "version")) {
		n = snprintf(msg, sizeof(msg), "xechide-kpm %s\n", "1.0.0");
	} else {
		n = snprintf(msg, sizeof(msg), "unknown command\n");
	}
	if (n < 0)
		return -EINVAL;
	if (n >= outlen)
		n = outlen > 0 ? outlen - 1 : 0;
	if (n > 0)
		compat_copy_to_user(out_msg, msg, n);
	return 0;
}

static long xechide_init(const char *args, const char *event, void *reserved)
{
	(void)args;
	(void)event;
	(void)reserved;
	resolve_symbols();
	if (load_config() == 0)
		pr_info("xechide-kpm: config loaded, %u rules\n", cfg_get()->n_rules);
	else
		pr_warn("xechide-kpm: no valid config at %s, hooks are pass-through\n",
			XE_CFG_PATH);
	install_hooks();
	return 0;
}

static long xechide_exit(void *reserved)
{
	(void)reserved;
	remove_hooks();
	pr_info("xechide-kpm: unloaded\n");
	return 0;
}

KPM_INIT(xechide_init);
KPM_CTL0(xechide_ctl);
KPM_EXIT(xechide_exit);
