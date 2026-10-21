/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * XEChide - kernel rule engine.
 *
 * The live configuration is a single kmalloc'd block published with RCU: every
 * hook does one rcu_dereference() and then a handful of string compares, so a
 * configuration update never blocks a file operation. Rules are validated
 * before the swap; a rejected update leaves the previous rules in force.
 */
#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/lockdep.h>
#include <linux/namei.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <linux/user_namespace.h>

#include "xechide.h"

bool xechide_enabled;
static struct xechide_config __rcu *xechide_cfg;
static DEFINE_SPINLOCK(xechide_cfg_lock);

static atomic_t xechide_stats[XE_STAT_MAX];

/* Hot-path copies of two config facts, refreshed after every successful SET.
 * READ_ONCE is enough: a stale value only costs one extra (or one missed)
 * hook call until the next configuration update. */
static unsigned int xe_n_rules;
static bool xe_self_hide;

/*
 * L0 self-hiding is cheap: instead of resolving a path for every file
 * operation of every uid, the superblock of /data/adb is cached once and the
 * hooks only pay for it when the inode actually lives there. A stale pointer
 * is only ever *compared*, never dereferenced.
 */
static struct super_block *xe_adb_sb;

static void xechide_cache_adb_sb(void)
{
	struct path path;

	if (xe_adb_sb || kern_path("/data/adb", LOOKUP_FOLLOW, &path))
		return;
	xe_adb_sb = path.dentry->d_sb;
	path_put(&path);
	pr_info("xechide: self-hide bound to /data/adb (sb %p)\n", xe_adb_sb);
}

bool xechide_on_adb_sb(const void *sb)
{
	return sb && xe_adb_sb && sb == xe_adb_sb;
}

bool xechide_self_hide_enabled(void)
{
	return READ_ONCE(xe_self_hide);
}

void xechide_stat_inc(int which)
{
	if (which >= 0 && which < XE_STAT_MAX)
		atomic_inc(&xechide_stats[which]);
}

static bool uid_protected(u32 uid)
{
	/* Never hide anything from root or system_server. */
	return uid == 0 || uid == 1000;
}

static bool rule_matches_uid(const struct xechide_config *cfg, const struct xechide_rule *rule, u32 uid)
{
	if (uid_protected(uid))
		return false;
	if (rule->target_uid == XECHIDE_UID_ALL) {
		/* uid 2000 is opt-in: hiding from the shell breaks tooling. */
		if (uid == 2000)
			return (cfg->flags & XECHIDE_CF_HIDE_SHELL) != 0;
		return true;
	}
	return rule->target_uid == uid;
}

bool xechide_uid_has_rules(u32 uid, u32 what)
{
	const struct xechide_config *cfg;
	bool hit = false;
	u32 i;

	if (!xechide_enabled || uid_protected(uid) || !READ_ONCE(xe_n_rules))
		return false;

	rcu_read_lock();
	cfg = rcu_dereference(xechide_cfg);
	if (!cfg)
		goto out;
	if (cfg->n_rules == 0)
		goto out;
	for (i = 0; i < cfg->n_rules; i++) {
		const struct xechide_rule *rule = &cfg->rules[i];

		if (!(rule->flags & what))
			continue;
		if (!rule_matches_uid(cfg, rule, uid))
			continue;
		hit = true;
		break;
	}
out:
	rcu_read_unlock();
	return hit;
}

/* ------------------------------------------------------------- self-hide --- */

static bool is_self_path(const char *path)
{
	static const char prefix[] = "/data/adb/xechide";
	size_t n = sizeof(prefix) - 1;

	return strncmp(path, prefix, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

/* ------------------------------------------------------------ /proc --- */

static bool parse_proc_pid(const char *path, u32 *pid_out)
{
	const char *p;
	u32 v = 0;

	if (strncmp(path, "/proc/", 6) != 0)
		return false;
	p = path + 6;
	if (*p < '0' || *p > '9')
		return false;
	while (*p >= '0' && *p <= '9') {
		if (v > 0xfffff)
			return false;
		v = v * 10 + (u32)(*p - '0');
		p++;
	}
	if (*p != '\0' && *p != '/')
		return false;
	if (!v)
		return false;
	*pid_out = v;
	return true;
}

/* uid of @nr as seen from the init namespace, 0 when it does not exist */
static u32 pid_uid(u32 nr)
{
	struct task_struct *task;
	struct pid *pid;
	u32 uid = 0;

	rcu_read_lock();
	pid = find_vpid(nr);
	if (pid) {
		task = pid_task(pid, PIDTYPE_PID);
		if (task)
			uid = from_kuid_munged(&init_user_ns, task_uid(task));
	}
	rcu_read_unlock();
	return uid;
}

/* ------------------------------------------------------------------ L0 --- */

bool xechide_hide_path(u32 uid, const char *path)
{
	const struct xechide_config *cfg;
	bool hidden = false, is_proc = false;
	u32 i;

	if (!xechide_enabled || uid_protected(uid) || !path || path[0] != '/')
		return false;

	rcu_read_lock();
	cfg = rcu_dereference(xechide_cfg);
	if (!cfg)
		goto out;

	if ((cfg->flags & XECHIDE_CF_HIDE_SELF) && is_self_path(path)) {
		hidden = true;
		goto out;
	}

	for (i = 0; i < cfg->n_rules; i++) {
		const struct xechide_rule *rule = &cfg->rules[i];
		u32 pid;

		if (!rule_matches_uid(cfg, rule, uid))
			continue;
		if ((rule->flags & XECHIDE_RF_PATHS) && xe_match_path(rule, path)) {
			hidden = true;
			break;
		}
		if ((rule->flags & XECHIDE_RF_PROC) && parse_proc_pid(path, &pid)) {
			u32 owner = pid_uid(pid);

			if (rule->hidden_uid && owner == rule->hidden_uid) {
				is_proc = true;
				hidden = true;
				break;
			}
		}
	}
out:
	rcu_read_unlock();
	if (hidden)
		xechide_stat_inc(is_proc ? XE_STAT_PROC : XE_STAT_PATH);
	return hidden;
}

bool xechide_hide_dirent(u32 uid, u32 dir, const char *name, int namelen)
{
	const struct xechide_config *cfg;
	bool hidden = false;
	u32 i;

	if (!xechide_enabled || uid_protected(uid) || !name || namelen <= 0)
		return false;

	rcu_read_lock();
	cfg = rcu_dereference(xechide_cfg);
	if (!cfg)
		goto out;

	for (i = 0; i < cfg->n_rules; i++) {
		const struct xechide_rule *rule = &cfg->rules[i];

		if (!rule_matches_uid(cfg, rule, uid))
			continue;
		if (dir == XE_DIR_PROC) {
			u32 pid = 0, k;

			if (!(rule->flags & XECHIDE_RF_PROC) || !rule->hidden_uid)
				continue;
			for (k = 0; k < (u32)namelen; k++) {
				if (name[k] < '0' || name[k] > '9')
					break;
				pid = pid * 10 + (u32)(name[k] - '0');
				if (pid > 0xfffff)
					break;
			}
			if (k != (u32)namelen || !pid)
				continue;
			if (pid_uid(pid) == rule->hidden_uid) {
				xechide_stat_inc(XE_STAT_PROC);
				hidden = true;
				break;
			}
			continue;
		}
		if ((rule->flags & XECHIDE_RF_PATHS) && xe_match_dirent(rule, dir, name, namelen)) {
			xechide_stat_inc(XE_STAT_DIRENT);
			hidden = true;
			break;
		}
	}
out:
	rcu_read_unlock();
	return hidden;
}

/*
 * Filter one parcel for @uid. The caller owns a private kernel copy; the
 * returned length is <= len and 0 means "leave the parcel alone".
 */
unsigned int xechide_filter_parcel(u32 uid, unsigned char *buf, unsigned int len, unsigned int *mode)
{
	const struct xechide_config *cfg;
	const char *names[XECHIDE_MAX_NAMES];
	unsigned int new_len = 0, n = 0;
	u32 i;

	*mode = XE_PARCEL_NONE;
	if (!xechide_enabled || uid_protected(uid) || !buf)
		return 0;

	rcu_read_lock();
	cfg = rcu_dereference(xechide_cfg);
	if (!cfg)
		goto out;
	for (i = 0; i < cfg->n_rules && n < XECHIDE_MAX_NAMES; i++) {
		const struct xechide_rule *rule = &cfg->rules[i];
		int j, dup = 0;

		if (!(rule->flags & XECHIDE_RF_BINDER))
			continue;
		if (!rule_matches_uid(cfg, rule, uid))
			continue;
		for (j = 0; j < (int)n; j++) {
			if (strcmp(names[j], (const char *)rule->pkg) == 0) {
				dup = 1;
				break;
			}
		}
		if (!dup)
			names[n++] = (const char *)rule->pkg;
	}
	if (!n)
		goto out;
	new_len = xe_parcel_filter(buf, len, names, (int)n, mode);
out:
	rcu_read_unlock();
	return new_len;
}

/* --------------------------------------------------------------- config --- */

static bool pkg_valid(const unsigned char *pkg)
{
	u32 i;

	for (i = 0; i < XECHIDE_PKG_LEN; i++) {
		unsigned char c = pkg[i];

		if (!c)
			return i > 0;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
		      c == '_'))
			return false;
	}
	return false; /* not NUL terminated */
}

static int cfg_validate(struct xechide_config *cfg)
{
	u32 i;

	if (cfg->magic != XECHIDE_ABI_MAGIC || cfg->version != XECHIDE_ABI_VERSION)
		return -EINVAL;
	if (cfg->n_rules > XECHIDE_MAX_RULES)
		return -EINVAL;
	cfg->flags &= XECHIDE_CF_HIDE_SHELL | XECHIDE_CF_HIDE_SELF;

	for (i = 0; i < cfg->n_rules; i++) {
		struct xechide_rule *rule = &cfg->rules[i];

		if (!pkg_valid(rule->pkg))
			return -EINVAL;
		if (rule->target_uid == 0 || rule->target_uid == 1000)
			return -EINVAL;
		if (rule->hidden_uid == 0 || rule->hidden_uid == 1000)
			return -EINVAL;
		rule->flags &= XECHIDE_RF_ALL;
		if (!rule->flags)
			return -EINVAL;
	}
	return 0;
}

int xechide_ioctl_set(void __user *arg)
{
	struct xechide_config *cfg, *old;

	if (!arg)
		return -EINVAL;
	cfg = kzalloc(sizeof(*cfg), GFP_KERNEL);
	if (!cfg)
		return -ENOMEM;
	if (copy_from_user(cfg, arg, sizeof(*cfg))) {
		kfree(cfg);
		return -EFAULT;
	}
	if (cfg_validate(cfg)) {
		kfree(cfg);
		return -EINVAL;
	}

	spin_lock(&xechide_cfg_lock);
	old = rcu_dereference_protected(xechide_cfg, lockdep_is_held(&xechide_cfg_lock));
	rcu_assign_pointer(xechide_cfg, cfg);
	WRITE_ONCE(xe_n_rules, cfg->n_rules);
	WRITE_ONCE(xe_self_hide, (cfg->flags & XECHIDE_CF_HIDE_SELF) != 0);
	spin_unlock(&xechide_cfg_lock);

	if (old) {
		synchronize_rcu();
		kfree(old);
	}
	xechide_cache_adb_sb();
	pr_info("xechide: config updated, %u rule(s), flags 0x%x\n", cfg->n_rules, cfg->flags);
	return 0;
}

int xechide_ioctl_get(void __user *arg)
{
	struct xechide_config *out;
	const struct xechide_config *cfg;
	int ret = 0;

	if (!arg)
		return -EINVAL;
	out = kzalloc(sizeof(*out), GFP_KERNEL);
	if (!out)
		return -ENOMEM;

	rcu_read_lock();
	cfg = rcu_dereference(xechide_cfg);
	if (cfg)
		memcpy(out, cfg, sizeof(*out));
	else {
		out->magic = XECHIDE_ABI_MAGIC;
		out->version = XECHIDE_ABI_VERSION;
	}
	rcu_read_unlock();

	if (copy_to_user(arg, out, sizeof(*out)))
		ret = -EFAULT;
	kfree(out);
	return ret;
}

void xechide_get_stats(struct xechide_stats *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	out->path_hits = (u32)atomic_read(&xechide_stats[XE_STAT_PATH]);
	out->dirent_hits = (u32)atomic_read(&xechide_stats[XE_STAT_DIRENT]);
	out->proc_hits = (u32)atomic_read(&xechide_stats[XE_STAT_PROC]);
	out->parcel_rebuilt = (u32)atomic_read(&xechide_stats[XE_STAT_REBUILT]);
	out->parcel_rewritten = (u32)atomic_read(&xechide_stats[XE_STAT_REWRITTEN]);
	out->parcel_skipped = (u32)atomic_read(&xechide_stats[XE_STAT_SKIPPED]);
	out->binder_errors = (u32)atomic_read(&xechide_stats[XE_STAT_BINDER]);
	out->lsm_hooks = (u32)atomic_read(&xechide_stats[XE_STAT_LSM]);
	out->binder_hooks = (u32)atomic_read(&xechide_stats[XE_STAT_BINDER_HOOKS]);
}

int __init xechide_init(void)
{
	xechide_enabled = true;
	xechide_cache_adb_sb();
	pr_info("xechide: rule engine ready (%u max rules)\n", XECHIDE_MAX_RULES);
	return 0;
}

void __exit xechide_remove(void)
{
	struct xechide_config *old;

	xechide_enabled = false;
	WRITE_ONCE(xe_n_rules, 0);
	WRITE_ONCE(xe_self_hide, false);
	spin_lock(&xechide_cfg_lock);
	old = rcu_dereference_protected(xechide_cfg, lockdep_is_held(&xechide_cfg_lock));
	rcu_assign_pointer(xechide_cfg, NULL);
	spin_unlock(&xechide_cfg_lock);
	if (old) {
		synchronize_rcu();
		kfree(old);
	}
	pr_info("xechide: rule engine stopped\n");
}
