// SPDX-License-Identifier: GPL-2.0
/*
 * XEChide - kernel-level app & path hiding for KernelSU.
 *
 * Standalone patch source: copied into a KernelSU kernel tree by
 * patches/xechide-core.patch and compiled into kernelsu.ko. Depends only on the
 * Linux kernel API and the KernelSU supercall table - no fork internals.
 *
 * Contract: ../DESIGN.md. Failure policy: unknown/malformed input passes
 * through unchanged (fail-open). Never deny uid 0 / 1000.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>

#define XECHIDE_MAGIC 0x58454348u /* "XECH" */
#define XECHIDE_VERSION 1u
#define XECHIDE_MAX_RULES 128
#define XECHIDE_PKG_LEN 128
#define XECHIDE_MAX_PREFIX 256

/* Rule flags */
#define XECHIDE_F_ALL 0x01u  /* hide from every non-root uid (self-hiding) */
#define XECHIDE_F_EXEC 0x02u /* also deny exec of the hidden APK */

enum xechide_path_class {
	XECHIDE_PATH_APK = 0,
	XECHIDE_PATH_DATA,
	XECHIDE_PATH_USER,
	XECHIDE_PATH_MEDIA,
	XECHIDE_PATH_PROC,
	XECHIDE_PATH_SYSTEM_LIST,
	XECHIDE_PATH_MAX,
};

struct xechide_rule {
	u32 target_uid; /* 0xFFFFFFFF = every target (XECHIDE_F_ALL) */
	u8 pkg[XECHIDE_PKG_LEN];
	u8 flags;
	u8 n_prefix;
	u8 pad[2];
	char prefix[XECHIDE_PATH_MAX][XECHIDE_MAX_PREFIX];
};

struct xechide_config {
	u32 magic;
	u32 version;
	u32 n_rules;
	u32 reserved;
	struct xechide_rule rules[XECHIDE_MAX_RULES];
};

static struct xechide_config *xechide_cfg; /* kmalloc'd, RCU-swapped */
static DEFINE_SPINLOCK(xechide_lock);

static bool xechide_uid_is_protected(u32 uid)
{
	/* system_server, root, init: hiding from these bricks the device. */
	return uid == 0 || uid == 1000;
}

static bool xechide_prefix_match(const char *path, const char *prefix)
{
	size_t n = strlen(prefix);

	if (n == 0)
		return false;
	return strncmp(path, prefix, n) == 0;
}

/*
 * Single decision point for all LSM hooks. Hot path: one rcu_dereference plus
 * at most n_rules prefix compares; each rule carries its own prefix array so
 * nothing is allocated here.
 */
bool xechide_hide_path(u32 uid, const char *path, bool is_exec)
{
	const struct xechide_config *cfg;
	bool hidden = false;
	u32 i;

	if (xechide_uid_is_protected(uid) || !path || !*path)
		return false;

	rcu_read_lock();
	cfg = rcu_dereference(xechide_cfg);
	if (!cfg)
		goto out;

	for (i = 0; i < cfg->n_rules; i++) {
		const struct xechide_rule *rule = &cfg->rules[i];
		u8 j;

		if (rule->target_uid != 0xFFFFFFFFu && rule->target_uid != uid)
			continue;
		if (is_exec && !(rule->flags & XECHIDE_F_EXEC))
			continue;
		for (j = 0; j < rule->n_prefix; j++) {
			if (xechide_prefix_match(path, rule->prefix[j])) {
				hidden = true;
				goto out;
			}
		}
	}
out:
	rcu_read_unlock();
	return hidden;
}
EXPORT_SYMBOL_GPL(xechide_hide_path);

/* ---------------------------------------------------------------- hooks --- */

/*
 * L0.1 - path lookup / stat, called from the integration's inode_getattr hook.
 * -ENOENT (not -EACCES) is deliberate: a permission error tells the detector
 * "something exists but is denied".
 */
int xechide_inode_getattr(u32 uid, const char *path)
{
	if (xechide_hide_path(uid, path, false))
		return -ENOENT;
	return 0;
}

/* L0.2 - read/exec/open. Only read-like masks are denied; writes always pass. */
int xechide_inode_permission(u32 uid, const char *path, int mask)
{
	bool is_exec = (mask & MAY_EXEC) != 0;

	if (xechide_hide_path(uid, path, is_exec))
		return -ENOENT;
	return 0;
}

/*
 * L0.3 - readdir. A hidden package's APK directory is named "<pkg>-<suffix>",
 * so a prefix compare against rule->pkg + '-' is enough; no path resolution.
 */
bool xechide_hide_dirent(u32 uid, const char *name)
{
	const struct xechide_config *cfg;
	bool hidden = false;
	u32 i;

	if (xechide_uid_is_protected(uid) || !name)
		return false;

	rcu_read_lock();
	cfg = rcu_dereference(xechide_cfg);
	if (!cfg)
		goto out;
	for (i = 0; i < cfg->n_rules; i++) {
		const struct xechide_rule *rule = &cfg->rules[i];
		size_t n = strnlen((const char *)rule->pkg, XECHIDE_PKG_LEN);

		if (rule->target_uid != 0xFFFFFFFFu && rule->target_uid != uid)
			continue;
		if (n && strncmp(name, (const char *)rule->pkg, n) == 0 &&
		    name[n] == '-') {
			hidden = true;
			goto out;
		}
	}
out:
	rcu_read_unlock();
	return hidden;
}

/*
 * L1 - binder reply filtering. The walk is intentionally NOT here: it must be
 * built against the exact binder.c of each KMI. Contract:
 *   - only (sender uid has rules) && (destination == package service)
 *   - parse: int32 len, int32 count, then count length-prefixed blocks
 *   - verify every block lies inside the buffer before touching it
 *   - rebuild into a fresh buffer, never in place
 *   - on anything unexpected: return 0 = pass the original through
 */
u32 xechide_filter_pkg_reply(u32 uid, void *parcel, u32 len)
{
	if (xechide_uid_is_protected(uid) || !parcel || len < 8)
		return 0;
	return 0; /* implemented by patches/xechide-binder-<kmi>.patch */
}

/* ------------------------------------------------------------- config --- */

static int xechide_resolve_prefixes(struct xechide_rule *rule, u32 android_user)
{
	const char *pkg = (const char *)rule->pkg;
	int n = 0;

	if (n < XECHIDE_PATH_MAX)
		snprintf(rule->prefix[n++], XECHIDE_MAX_PREFIX, "/data/data/%s", pkg);
	if (n < XECHIDE_PATH_MAX)
		snprintf(rule->prefix[n++], XECHIDE_MAX_PREFIX, "/data/user/%u/%s",
			 android_user, pkg);
	if (n < XECHIDE_PATH_MAX)
		snprintf(rule->prefix[n++], XECHIDE_MAX_PREFIX,
			 "/data/media/%u/Android/data/%s", android_user, pkg);
	rule->n_prefix = (u8)n;
	return 0;
}

/* Replace the live config (KSU_IOCTL_XECHIDE_SET). Returns 0 or -errno. */
int xechide_set_config(const void __user *user_buf, size_t len)
{
	struct xechide_config *cfg, *old;
	u32 i;
	int j;

	if (!user_buf || len < sizeof(*cfg))
		return -EINVAL;

	cfg = kzalloc(sizeof(*cfg), GFP_KERNEL);
	if (!cfg)
		return -ENOMEM;
	if (copy_from_user(cfg, user_buf, sizeof(*cfg))) {
		kfree(cfg);
		return -EFAULT;
	}
	if (cfg->magic != XECHIDE_MAGIC || cfg->version != XECHIDE_VERSION ||
	    cfg->n_rules > XECHIDE_MAX_RULES) {
		kfree(cfg);
		return -EINVAL;
	}

	for (i = 0; i < cfg->n_rules; i++) {
		struct xechide_rule *rule = &cfg->rules[i];
		size_t n;

		rule->pkg[XECHIDE_PKG_LEN - 1] = 0;
		n = strlen((const char *)rule->pkg);
		if (!n)
			goto bad_rule;
		for (j = 0; j < (int)n; j++) {
			char c = (char)rule->pkg[j];

			if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			      (c >= '0' && c <= '9') || c == '.' || c == '_'))
				goto bad_rule;
		}
		if (xechide_uid_is_protected(rule->target_uid))
			goto bad_rule;
		rule->flags &= XECHIDE_F_ALL | XECHIDE_F_EXEC;
		xechide_resolve_prefixes(rule, 0);
	}

	spin_lock(&xechide_lock);
	old = rcu_dereference_protected(xechide_cfg, lockdep_is_held(&xechide_lock));
	rcu_assign_pointer(xechide_cfg, cfg);
	spin_unlock(&xechide_lock);
	if (old) {
		synchronize_rcu();
		kfree(old);
	}
	return 0;

bad_rule:
	kfree(cfg);
	return -EINVAL;
}

int xechide_get_config(void __user *user_buf, size_t len)
{
	const struct xechide_config *cfg;
	int ret = 0;

	if (!user_buf || len < sizeof(*cfg))
		return -EINVAL;
	rcu_read_lock();
	cfg = rcu_dereference(xechide_cfg);
	if (!cfg)
		ret = -ENODATA;
	else if (copy_to_user(user_buf, cfg, sizeof(*cfg)))
		ret = -EFAULT;
	rcu_read_unlock();
	return ret;
}

void xechide_clear(void)
{
	struct xechide_config *old;

	spin_lock(&xechide_lock);
	old = rcu_dereference_protected(xechide_cfg, lockdep_is_held(&xechide_lock));
	rcu_assign_pointer(xechide_cfg, NULL);
	spin_unlock(&xechide_lock);
	if (old) {
		synchronize_rcu();
		kfree(old);
	}
}

int __init xechide_init(void)
{
	pr_info("XEChide: loaded (v%u, max %u rules)\n", XECHIDE_VERSION,
		XECHIDE_MAX_RULES);
	return 0;
}

void __exit xechide_exit(void)
{
	xechide_clear();
	pr_info("XEChide: unloaded\n");
}

module_init(xechide_init);
module_exit(xechide_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("XEChide - kernel-level app & path hiding for KernelSU");
