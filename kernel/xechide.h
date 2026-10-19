/* SPDX-License-Identifier: GPL-2.0 */
/*
 * XEChide - kernel-level app & path hiding.
 *
 * This header is the ABI between the kernel part, the root helper (ctl/) and
 * the Android app (manager/). It is deliberately self-contained: no kernel
 * headers, so userspace can include it verbatim. All integers are 32/8 bit and
 * every structure is packed, so the layout is identical on arm64 and x86_64.
 */
#ifndef _XECHIDE_H
#define _XECHIDE_H

#define XECHIDE_ABI_MAGIC 0x58454348u /* "XECH" */
#define XECHIDE_ABI_VERSION 1u

#define XECHIDE_MAX_RULES 64
#define XECHIDE_PKG_LEN 128

/* target_uid: this rule applies to every uid except 0 and 1000. */
#define XECHIDE_UID_ALL 0xffffffffu

/* per-rule flags */
#define XECHIDE_RF_PATHS 0x0001u  /* hide the app's files (L0) */
#define XECHIDE_RF_PROC 0x0002u   /* hide the app's processes (L0) */
#define XECHIDE_RF_BINDER 0x0004u /* filter PackageManager replies (L1) */
#define XECHIDE_RF_ALL (XECHIDE_RF_PATHS | XECHIDE_RF_PROC | XECHIDE_RF_BINDER)

/* global flags */
#define XECHIDE_CF_HIDE_SHELL 0x0001u /* also hide from adb shell (uid 2000) */
#define XECHIDE_CF_HIDE_SELF 0x0002u  /* hide /data/adb/xechide from non-root */

struct xechide_rule {
	unsigned int target_uid; /* who must not see the app, or XECHIDE_UID_ALL */
	unsigned int hidden_uid; /* the app's uid, used for /proc hiding */
	unsigned int flags;      /* XECHIDE_RF_* */
	unsigned char pkg[XECHIDE_PKG_LEN];
} __attribute__((packed));

struct xechide_config {
	unsigned int magic;
	unsigned int version;
	unsigned int n_rules;
	unsigned int flags;
	struct xechide_rule rules[XECHIDE_MAX_RULES];
} __attribute__((packed));

/* Parcel filter outcome (xechide_parcel.c). */
#define XE_PARCEL_NONE 0u
#define XE_PARCEL_REBUILT 1u   /* elements removed, count updated */
#define XE_PARCEL_REWRITTEN 2u /* layout untouched, names renamed */

/* Directory classes for readdir filtering. */
#define XE_DIR_APP 0u   /* /data/app */
#define XE_DIR_DATA 1u  /* /data/data, /data/user/<n> */
#define XE_DIR_PROC 2u  /* /proc */
#define XE_DIR_MEDIA 3u /* Android/data on external storage */
#define XE_DIR_NONE 0xffu

/*
 * Pure helpers, shared by the kernel module and the host test suite
 * (tests/host). Return int, not bool, so userspace does not need stdbool
 * before including this header.
 */
int xe_match_path(const struct xechide_rule *rule, const char *path);
int xe_match_dirent(const struct xechide_rule *rule, unsigned int dir, const char *name, int namelen);
unsigned int xe_parcel_filter(unsigned char *buf, unsigned int len, const char *const *names, int n_names,
			      unsigned int *mode);

/* Counters, read with KSU_IOCTL_XECHIDE_GET's stats magic. */
struct xechide_stats {
	unsigned int path_hits;
	unsigned int dirent_hits;
	unsigned int proc_hits;
	unsigned int parcel_rebuilt;   /* replies rewritten with entries removed */
	unsigned int parcel_rewritten; /* replies rewritten in place (name swap) */
	unsigned int parcel_skipped;   /* replies we refused to touch */
	unsigned int binder_errors;    /* marks that never got a reply */
	unsigned int lsm_hooks;        /* hooks that actually installed */
	unsigned int binder_hooks;     /* binder probes that actually installed */
};

#ifdef __KERNEL__

#include <linux/types.h>
#include <linux/uidgid.h>

/* names collected for one parcel filter */
#define XECHIDE_MAX_NAMES 16

enum xechide_stat {
	XE_STAT_PATH = 0,
	XE_STAT_DIRENT,
	XE_STAT_PROC,
	XE_STAT_REBUILT,
	XE_STAT_REWRITTEN,
	XE_STAT_SKIPPED,
	XE_STAT_BINDER,
	XE_STAT_LSM,
	XE_STAT_BINDER_HOOKS,
	XE_STAT_MAX,
};

/* xechide_core.c */
bool xechide_uid_has_rules(u32 uid, u32 what);
bool xechide_self_hide_enabled(void);
bool xechide_on_adb_sb(const void *sb);
bool xechide_hide_path(u32 uid, const char *path);
bool xechide_hide_dirent(u32 uid, u32 dir, const char *name, int namelen);
unsigned int xechide_filter_parcel(u32 uid, unsigned char *buf, unsigned int len, unsigned int *mode);
void xechide_stat_inc(int which);
void xechide_get_stats(struct xechide_stats *out);
int xechide_init(void);
void xechide_remove(void);

/* xechide_binder.c */
int xechide_binder_init(void);
void xechide_binder_exit(void);

/* xechide_lsm.c */
int xechide_lsm_init(void);
void xechide_lsm_exit(void);

/* supercall handlers */
int xechide_ioctl_get(void __user *arg);
int xechide_ioctl_set(void __user *arg);

extern bool xechide_enabled;

#endif /* __KERNEL__ */
#endif /* _XECHIDE_H */
