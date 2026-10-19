/* SPDX-License-Identifier: GPL-2.0 */
/*
 * XEChide - L0 enforcement.
 *
 * Hooks are installed by replacing the SELinux slot of four LSM entries
 * through KernelSU's ksu_lsm_hook() (the same mechanism KernelSU itself uses
 * for selinux_setprocattr). Our replacement runs first and calls the original
 * SELinux handler for everything it does not deny, so SELinux enforcement is
 * never weakened.
 *
 * Readdir filtering cannot be done with an LSM hook (there is no
 * inode_readdir hook), so `iterate_dir` is instrumented with a kretprobe and
 * the filling actor of the directory context is swapped for the duration of
 * one readdir. The wrapper either drops a name or forwards it to the original
 * actor - the filesystem itself is untouched.
 *
 * Every failure path here is fail-open: a hook that cannot be installed, a path
 * that cannot be resolved, or a context that cannot be tracked leaves the
 * kernel's normal behaviour in place.
 */
#include <linux/cred.h>
#include <linux/dcache.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/lsm_hooks.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uidgid.h>
#include <linux/version.h>

#include "arch.h"
#include "hook/lsm_hook.h"
#include "klog.h"
#include "xechide.h"

#ifndef MAY_ACCESS
#define MAY_ACCESS 0x10
#endif

#define XE_PATH_MAX 512

/* --------------------------------------------------------------- helpers --- */

/* Path of @dentry relative to the root, or NULL. No allocation, no sleep. */
static const char *xe_dentry_path(struct dentry *d, char *buf, int len)
{
	char *p;

	if (!d || !buf || len <= 0)
		return NULL;
	p = dentry_path_raw(d, buf, len);
	if (IS_ERR_OR_NULL(p) || p[0] != '/')
		return NULL;
	return p;
}

static bool is_digits(const char *p, const char **end)
{
	const char *s = p;

	while (*p >= '0' && *p <= '9')
		p++;
	if (p == s)
		return false;
	if (end)
		*end = p;
	return true;
}

/* "/data/data", "/data/user/<n>", "/data/user_de/<n>" */
static bool is_data_dir(const char *path)
{
	static const char ud[] = "/data/user/";
	static const char udd[] = "/data/user_de/";
	const char *end;

	if (!strcmp(path, "/data/data"))
		return true;
	if (!strncmp(path, ud, sizeof(ud) - 1)) {
		if (!is_digits(path + sizeof(ud) - 1, &end) || *end)
			return false;
		return true;
	}
	if (!strncmp(path, udd, sizeof(udd) - 1)) {
		if (!is_digits(path + sizeof(udd) - 1, &end) || *end)
			return false;
		return true;
	}
	return false;
}

static bool is_media_dir(const char *path)
{
	size_t n = strlen(path);
	static const char suffix[] = "/Android/data";
	size_t m = sizeof(suffix) - 1;

	return n >= m && strcmp(path + n - m, suffix) == 0;
}

static unsigned int dir_kind(struct file *file, char *buf, int len)
{
	const char *path;

	if (!file || !file->f_path.dentry)
		return XE_DIR_NONE;
	path = xe_dentry_path(file->f_path.dentry, buf, len);
	if (!path)
		return XE_DIR_NONE;
	if (!strcmp(path, "/data/app"))
		return XE_DIR_APP;
	if (is_data_dir(path))
		return XE_DIR_DATA;
	if (!strcmp(path, "/proc"))
		return XE_DIR_PROC;
	if (is_media_dir(path))
		return XE_DIR_MEDIA;
	return XE_DIR_NONE;
}

/* Cheap gate for every hook: a uid with rules, or a filesystem that carries
 * the project's own hidden path while self-hiding is enabled. */
static bool xe_active(u32 uid, const struct super_block *sb)
{
	if (xechide_uid_has_rules(uid, XECHIDE_RF_PATHS | XECHIDE_RF_PROC))
		return true;
	return xechide_self_hide_enabled() && xechide_on_adb_sb(sb);
}

/* ------------------------------------------------------------- L0 hooks --- */

/* Filled in by xechide_lsm_init(); the originals are called for everything we
 * do not deny, so SELinux keeps enforcing. */
static struct ksu_lsm_hook xe_inode_permission_hook, xe_inode_getattr_hook, xe_file_permission_hook,
	xe_file_open_hook;

/* lsm_hook_defs.h keeps this signature from 5.10 through 6.18. */
typedef int (*inode_permission_fn)(struct inode *inode, int mask);

static int xe_inode_permission(struct inode *inode, int mask)
{	u32 uid = current_uid().val;
	char buf[XE_PATH_MAX];
	struct dentry *d;
	int ret;

	if (!xechide_enabled || !(mask & (MAY_READ | MAY_EXEC | MAY_ACCESS)))
		goto orig;
	if (!xe_active(uid, inode->i_sb))
		goto orig;

	d = d_find_alias(inode);
	if (!d)
		goto orig;
	{
		const char *path = xe_dentry_path(d, buf, sizeof(buf));
		bool hidden = path && xechide_hide_path(uid, path);

		dput(d);
		if (hidden) {
			return -ENOENT;
		}
	}
orig:
	if (!xe_inode_permission_hook.original)
		return 0;
	ret = ((inode_permission_fn)xe_inode_permission_hook.original)(inode, mask);
	return ret;
}

typedef int (*inode_getattr_fn)(const struct path *path);

static int xe_inode_getattr(const struct path *path)
{
	u32 uid = current_uid().val;
	char buf[XE_PATH_MAX];

	if (xechide_enabled && path && xe_active(uid, path->dentry->d_sb)) {
		const char *p = xe_dentry_path(path->dentry, buf, sizeof(buf));

		if (p && xechide_hide_path(uid, p)) {
			return -ENOENT;
		}
	}
	if (!xe_inode_getattr_hook.original)
		return 0;
	return ((inode_getattr_fn)xe_inode_getattr_hook.original)(path);
}

typedef int (*file_permission_fn)(struct file *file, int mask);

static int xe_file_permission(struct file *file, int mask)
{
	u32 uid = current_uid().val;
	char buf[XE_PATH_MAX];

	if (xechide_enabled && file && (mask & (MAY_READ | MAY_EXEC)) &&
	    xe_active(uid, file->f_path.dentry->d_sb)) {
		const char *p = xe_dentry_path(file->f_path.dentry, buf, sizeof(buf));

		if (p && xechide_hide_path(uid, p)) {
			return -ENOENT;
		}
	}
	if (!xe_file_permission_hook.original)
		return 0;
	return ((file_permission_fn)xe_file_permission_hook.original)(file, mask);
}

typedef int (*file_open_fn)(struct file *file);

static int xe_file_open(struct file *file)
{
	u32 uid = current_uid().val;
	char buf[XE_PATH_MAX];

	if (xechide_enabled && file && xe_active(uid, file->f_path.dentry->d_sb)) {
		const char *p = xe_dentry_path(file->f_path.dentry, buf, sizeof(buf));

		if (p && xechide_hide_path(uid, p)) {
			return -ENOENT;
		}
	}
	if (!xe_file_open_hook.original)
		return 0;
	return ((file_open_fn)xe_file_open_hook.original)(file);
}

/* -------------------------------------------------------- readdir filter --- */

struct xe_dir_slot {
	struct dir_context *ctx;
	void *actor;
	u32 uid;
	u32 kind;
};

struct xe_dir_state {
	struct dir_context *ctx;
	u8 swapped;
};

#define XE_DIR_SLOTS 32
static struct xe_dir_slot xe_dir_slots[XE_DIR_SLOTS];
static DEFINE_SPINLOCK(xe_dir_lock);

static struct xe_dir_slot *xe_slot_of(struct dir_context *ctx)
{
	int i;

	for (i = 0; i < XE_DIR_SLOTS; i++)
		if (xe_dir_slots[i].ctx == ctx)
			return &xe_dir_slots[i];
	return NULL;
}

static int xe_dir_actor(struct dir_context *ctx, const char *name, int namelen, loff_t offset, u64 ino,
			unsigned int d_type)
{
	struct xe_dir_slot slot;
	unsigned long flags;
	struct xe_dir_slot *found;
	filldir_t orig;

	spin_lock_irqsave(&xe_dir_lock, flags);
	found = xe_slot_of(ctx);
	if (found)
		slot = *found;
	spin_unlock_irqrestore(&xe_dir_lock, flags);
	if (!found)
		return 0; /* untracked context: drop the entry rather than call NULL */

	if (xechide_hide_dirent(slot.uid, slot.kind, name, namelen))
		return 0; /* skipped: the filesystem treats 0 as "next entry" */

	orig = (filldir_t)slot.actor;
	if (!orig)
		return 0;
	return orig(ctx, name, namelen, offset, ino, d_type);
}

static int xe_iter_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct xe_dir_state *st = (struct xe_dir_state *)ri->data;
	struct dir_context *ctx = (struct dir_context *)PT_REGS_PARM2(regs);
	struct file *file = (struct file *)PT_REGS_PARM1(regs);
	unsigned long flags;
	struct xe_dir_slot *slot;
	char buf[XE_PATH_MAX];
	unsigned int kind;
	u32 uid = current_uid().val;

	st->ctx = NULL;
	st->swapped = 0;
	if (!xechide_enabled || !ctx || !ctx->actor || !file)
		return 1;
	if (!xe_active(uid, file->f_path.dentry->d_sb))
		return 1;

	kind = dir_kind(file, buf, sizeof(buf));
	if (kind == XE_DIR_NONE)
		return 1;

	spin_lock_irqsave(&xe_dir_lock, flags);
	slot = NULL;
	{
		int i;

		for (i = 0; i < XE_DIR_SLOTS; i++) {
			if (xe_dir_slots[i].ctx == NULL) {
				slot = &xe_dir_slots[i];
				break;
			}
		}
	}
	if (!slot) {
		spin_unlock_irqrestore(&xe_dir_lock, flags);
		return 1;
	}
	slot->ctx = ctx;
	slot->actor = (void *)ctx->actor;
	slot->uid = uid;
	slot->kind = kind;
	ctx->actor = xe_dir_actor;
	spin_unlock_irqrestore(&xe_dir_lock, flags);

	st->ctx = ctx;
	st->swapped = 1;
	return 0;
}

static int xe_iter_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct xe_dir_state *st = (struct xe_dir_state *)ri->data;
	unsigned long flags;

	(void)regs;
	if (!st->swapped || !st->ctx)
		return 0;
	spin_lock_irqsave(&xe_dir_lock, flags);
	{
		struct xe_dir_slot *slot = xe_slot_of(st->ctx);

		if (slot)
			memset(slot, 0, sizeof(*slot));
	}
	spin_unlock_irqrestore(&xe_dir_lock, flags);
	return 0;
}

static struct kretprobe xe_iterate_rp = {
	.kp.symbol_name = "iterate_dir",
	.entry_handler = xe_iter_entry,
	.handler = xe_iter_ret,
	.data_size = sizeof(struct xe_dir_state),
	.maxactive = 64,
};

/* --------------------------------------------------------- hook install --- */

int xechide_lsm_init(void)
{
	int installed = 0, ret;

	/* Compound literals: the hook structs are declared above the handlers so
	 * that a handler can call hook.original without a global lookup. */
	xe_inode_permission_hook =
		(struct ksu_lsm_hook)KSU_LSM_HOOK_INIT(inode_permission, "selinux_inode_permission", xe_inode_permission, 0);
	xe_inode_getattr_hook =
		(struct ksu_lsm_hook)KSU_LSM_HOOK_INIT(inode_getattr, "selinux_inode_getattr", xe_inode_getattr, 0);
	xe_file_permission_hook =
		(struct ksu_lsm_hook)KSU_LSM_HOOK_INIT(file_permission, "selinux_file_permission", xe_file_permission, 0);
	xe_file_open_hook =
		(struct ksu_lsm_hook)KSU_LSM_HOOK_INIT(file_open, "selinux_file_open", xe_file_open, 0);

	ret = ksu_lsm_hook(&xe_inode_permission_hook);
	if (ret) {
		pr_warn("xechide: inode_permission hook failed: %d\n", ret);
	} else {
		installed++;
		xechide_stat_inc(XE_STAT_LSM);
	}

	ret = ksu_lsm_hook(&xe_inode_getattr_hook);
	if (ret) {
		pr_warn("xechide: inode_getattr hook failed: %d\n", ret);
	} else {
		installed++;
		xechide_stat_inc(XE_STAT_LSM);
	}

	ret = ksu_lsm_hook(&xe_file_permission_hook);
	if (ret) {
		pr_warn("xechide: file_permission hook failed: %d\n", ret);
	} else {
		installed++;
		xechide_stat_inc(XE_STAT_LSM);
	}

	ret = ksu_lsm_hook(&xe_file_open_hook);
	if (ret) {
		pr_warn("xechide: file_open hook failed: %d\n", ret);
	} else {
		installed++;
		xechide_stat_inc(XE_STAT_LSM);
	}

	ret = register_kretprobe(&xe_iterate_rp);
	if (ret) {
		pr_warn("xechide: iterate_dir probe failed: %d\n", ret);
	} else {
		installed++;
		xechide_stat_inc(XE_STAT_LSM);
	}

	pr_info("xechide: %d/%d L0 hooks installed\n", installed, 5);
	return installed ? 0 : -ENOSYS;
}

void xechide_lsm_exit(void)
{
	unregister_kretprobe(&xe_iterate_rp);
	ksu_lsm_unhook(&xe_file_open_hook);
	ksu_lsm_unhook(&xe_file_permission_hook);
	ksu_lsm_unhook(&xe_inode_getattr_hook);
	ksu_lsm_unhook(&xe_inode_permission_hook);
}
