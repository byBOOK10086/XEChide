/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * XEChide - L1 enforcement (binder).
 *
 * Two probes, no binder-internal struct layout, no KMI-specific code:
 *
 *   binder_transaction (request) - current is the caller, so current_uid() is
 *                                  the target uid. If the parcel carries one of
 *                                  the system-service interface tokens, the
 *                                  thread is marked and hidden package names
 *                                  inside the request are renamed in place.
 *
 *   binder_thread_read (reply)   - current is the caller again. If the thread
 *                                  is marked and the buffer contains BR_REPLY,
 *                                  the reply parcel is rewritten (entries
 *                                  removed, count fixed) or the names are
 *                                  renamed. Only the parcel length field is
 *                                  updated.
 *
 * Everything runs on a private copy and only UAPI structures are touched, so a
 * mismatch can never corrupt a parcel: the filter either validates the whole
 * container or leaves it byte-identical.
 */
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <uapi/linux/android/binder.h>

#include "arch.h"
#include "klog.h"
#include "xechide.h"

#define XE_PARCEL_MAX (256u * 1024u)
#define XE_TOKEN_SCAN 1024u
#define XE_MARKS 64
#define XE_MARK_TTL (10 * HZ)

static unsigned char *xe_parcel_buf;
static DEFINE_SPINLOCK(xe_buf_lock);

struct xe_mark {
	u32 pid;
	u32 uid;
	unsigned long expires;
};

static struct xe_mark xe_marks[XE_MARKS];
static DEFINE_SPINLOCK(xe_mark_lock);

/* --------------------------------------------------------------- marks --- */

static void mark_add(u32 pid, u32 uid)
{
	unsigned long flags, now = jiffies;
	int i, free_slot = -1;

	spin_lock_irqsave(&xe_mark_lock, flags);
	for (i = 0; i < XE_MARKS; i++) {
		if (xe_marks[i].pid == pid && time_before(now, xe_marks[i].expires)) {
			xe_marks[i].uid = uid;
			xe_marks[i].expires = now + XE_MARK_TTL;
			spin_unlock_irqrestore(&xe_mark_lock, flags);
			return;
		}
		if (free_slot < 0 && (xe_marks[i].pid == 0 || time_after(now, xe_marks[i].expires)))
			free_slot = i;
	}
	if (free_slot >= 0) {
		xe_marks[free_slot].pid = pid;
		xe_marks[free_slot].uid = uid;
		xe_marks[free_slot].expires = now + XE_MARK_TTL;
	}
	spin_unlock_irqrestore(&xe_mark_lock, flags);
}

static bool mark_peek(u32 pid, u32 *uid)
{
	unsigned long flags;
	bool found = false;
	int i;

	spin_lock_irqsave(&xe_mark_lock, flags);
	for (i = 0; i < XE_MARKS; i++) {
		if (xe_marks[i].pid == pid && time_before(jiffies, xe_marks[i].expires)) {
			*uid = xe_marks[i].uid;
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&xe_mark_lock, flags);
	return found;
}

static void mark_remove(u32 pid)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&xe_mark_lock, flags);
	for (i = 0; i < XE_MARKS; i++) {
		if (xe_marks[i].pid == pid) {
			memset(&xe_marks[i], 0, sizeof(xe_marks[i]));
			break;
		}
	}
	spin_unlock_irqrestore(&xe_mark_lock, flags);
}

/* ------------------------------------------------------------- helpers --- */

static inline u32 rd32(const unsigned char *p)
{
	u32 v;

	memcpy(&v, p, 4);
	return v;
}

static bool zeros_after(const unsigned char *p, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		if (p[i])
			return false;
	return true;
}

/* Does @buf contain @token as a parcel string (String16 or String8)? */
static bool parcel_has_token(const unsigned char *buf, unsigned int len, const char *token)
{
	unsigned int n = (unsigned int)strlen(token), off, total, i;

	if (len < 8 || !n)
		return false;
	if (len > XE_TOKEN_SCAN)
		len = XE_TOKEN_SCAN;

	for (off = 0; off + 4 <= len; off++) {
		if (rd32(buf + off) != n)
			continue;
		total = (4u + 2u * n + 2u + 3u) & ~3u;
		if (off + total <= len) {
			bool ok = true;

			for (i = 0; i < n; i++) {
				if (buf[off + 4 + 2 * i] != (unsigned char)token[i] || buf[off + 5 + 2 * i] != 0) {
					ok = false;
					break;
				}
			}
			if (ok && zeros_after(buf + off + 4 + 2 * n, total - 4 - 2 * n))
				return true;
		}
		total = (4u + n + 1u + 3u) & ~3u;
		if (off + total <= len && memcmp(buf + off + 4, token, n) == 0 &&
		    zeros_after(buf + off + 4 + n, total - 4 - n))
			return true;
	}
	return false;
}

/*
 * System services whose parcels may carry a package name. The interface token
 * is the first string of every AIDL request, so matching it costs one bounded
 * scan and needs no transaction-number table.
 */
static const char *const xe_service_tokens[] = {
	/* package + activity surfaces */
	"android.content.pm.IPackageManager",
	"android.app.IActivityManager",
	"android.app.IActivityTaskManager",
	"android.content.pm.ILauncherApps",
	"android.content.pm.IPackageInstaller",
	"android.content.pm.IShortcutService",
	/* usage / ops / jobs */
	"android.app.usage.IUsageStatsManager",
	"com.android.internal.app.IAppOpsService",
	"android.app.job.IJobScheduler",
	/* notifications and windows */
	"android.app.INotificationManager",
	"android.view.IWindowManager",
	"android.view.accessibility.IAccessibilityManager",
};

static bool parcel_names_service(const unsigned char *buf, unsigned int len)
{
	int i;

	for (i = 0; i < (int)ARRAY_SIZE(xe_service_tokens); i++)
		if (parcel_has_token(buf, len, xe_service_tokens[i]))
			return true;
	return false;
}

/* --------------------------------------------------------- request probe --- */

static int xe_txn_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct binder_transaction_data *tr = (struct binder_transaction_data *)PT_REGS_PARM3(regs);
	unsigned long flags;
	unsigned int mode = 0, new_len, size;
	u32 uid = current_uid().val;
	int reply = (int)PT_REGS_CCALL_PARM4(regs);

	if (reply)
		return 0; /* replies are handled on the read side */
	if (!tr || !tr->data_size || !tr->data.ptr.buffer)
		return 0;
	if (!xechide_uid_has_rules(uid, XECHIDE_RF_BINDER))
		return 0;

	size = (unsigned int)tr->data_size;
	if (size < 8 || size > XE_PARCEL_MAX)
		return 0;

	if (!spin_trylock_irqsave(&xe_buf_lock, flags))
		return 0;
	/*
	 * Most transactions from a target uid are not PackageManager calls. Read
	 * only the head first: the interface token is the first string of the
	 * parcel, so a 1 KiB probe answers the question without copying the whole
	 * payload.
	 */
	{
		unsigned int head = size < XE_TOKEN_SCAN ? size : XE_TOKEN_SCAN;

		if (copy_from_user_nofault(xe_parcel_buf, (void __user *)tr->data.ptr.buffer, head)) {
			spin_unlock_irqrestore(&xe_buf_lock, flags);
			return 0;
		}
		if (!parcel_names_service(xe_parcel_buf, head)) {
			spin_unlock_irqrestore(&xe_buf_lock, flags);
			return 0;
		}
		if (head < size &&
		    copy_from_user_nofault(xe_parcel_buf + head, (char __user *)tr->data.ptr.buffer + head,
					   size - head)) {
			spin_unlock_irqrestore(&xe_buf_lock, flags);
			return 0;
		}
	}

	new_len = xechide_filter_parcel(uid, xe_parcel_buf, size, &mode);
	if (mode == XE_PARCEL_REWRITTEN && new_len == size) {
		/* Layout untouched: write the renamed request back. */
		if (copy_to_user_nofault((void __user *)tr->data.ptr.buffer, xe_parcel_buf, size))
			pr_debug("xechide: request writeback failed\n");
	}
	spin_unlock_irqrestore(&xe_buf_lock, flags);

	mark_add(task_pid_nr(current), uid);
	return 0;
}

static bool xe_txn_registered;

static struct kprobe xe_txn_kp = {
	.symbol_name = "binder_transaction",
	.pre_handler = xe_txn_pre,
};

/* ----------------------------------------------------------- reply probe --- */

struct xe_read_state {
	unsigned long buffer;
	unsigned long consumed;
};

static int xe_read_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct xe_read_state *st = (struct xe_read_state *)ri->data;

	st->buffer = PT_REGS_PARM3(regs);
	st->consumed = PT_REGS_PARM5(regs);
	return 0;
}

/*
 * Walk the read buffer. Only the commands binder can queue in front of a reply
 * are understood; anything else stops the walk (fail-open).
 * Returns true when a reply parcel was rewritten.
 */
static bool filter_reply(u32 uid, void __user *buffer, unsigned int used)
{
	unsigned int off = 0;
	bool filtered = false;
	unsigned long flags;

	while (off + 4 <= used) {
		u32 cmd;

		if (copy_from_user_nofault(&cmd, (char __user *)buffer + off, 4))
			break;

		if (cmd == BR_REPLY) {
			struct binder_transaction_data trd;
			unsigned int total = 4u + sizeof(trd);
			unsigned int new_len, mode = 0;

			if (off + total > used)
				break;
			if (copy_from_user_nofault(&trd, (char __user *)buffer + off + 4, sizeof(trd)))
				break;
			if (!trd.data_size || trd.data_size > XE_PARCEL_MAX || trd.offsets_size ||
			    !trd.data.ptr.buffer)
				break;

			if (!spin_trylock_irqsave(&xe_buf_lock, flags))
				break;
			if (copy_from_user_nofault(xe_parcel_buf, (void __user *)trd.data.ptr.buffer, trd.data_size)) {
				spin_unlock_irqrestore(&xe_buf_lock, flags);
				break;
			}
			new_len = xechide_filter_parcel(uid, xe_parcel_buf, trd.data_size, &mode);
			if (new_len && new_len <= trd.data_size &&
			    !copy_to_user_nofault((void __user *)trd.data.ptr.buffer, xe_parcel_buf, new_len)) {
				if (new_len != trd.data_size) {
					unsigned int at = off + 4 + (unsigned int)offsetof(struct binder_transaction_data,
											   data_size);
					u32 new_size = new_len;

					if (!copy_to_user_nofault((char __user *)buffer + at, &new_size, 4))
						filtered = true;
				} else {
					filtered = true;
				}
			}
			spin_unlock_irqrestore(&xe_buf_lock, flags);
			return filtered;
		}

		switch (cmd) {
		case BR_NOOP:
		case BR_TRANSACTION_COMPLETE:
		case BR_SPAWN_LOOPER:
		case BR_FAILED_REPLY:
		case BR_DEAD_REPLY:
		case BR_OK:
		case BR_ONEWAY_SPAM_SUSPECT:
			off += 4;
			break;
		case BR_ERROR:
			off += 8;
			break;
		case BR_INCREFS:
		case BR_ACQUIRE:
		case BR_RELEASE:
		case BR_DECREFS:
			off += 4 + 16;
			break;
		case BR_DEAD_BINDER:
		case BR_CLEAR_DEATH_NOTIFICATION_DONE:
			off += 4 + 8;
			break;
		default:
			return false;
		}
	}
	return false;
}

static int xe_read_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct xe_read_state *st = (struct xe_read_state *)ri->data;
	unsigned int used = 0;
	u32 uid = 0, pid;

	if (regs_return_value(regs))
		return 0;
	if (!st->buffer || !st->consumed)
		return 0;

	pid = task_pid_nr(current);
	if (!mark_peek(pid, &uid))
		return 0;
	if (!xechide_uid_has_rules(uid, XECHIDE_RF_BINDER)) {
		mark_remove(pid);
		return 0;
	}
	if (copy_from_user_nofault(&used, (void __user *)st->consumed, 4))
		return 0;
	if (!used || used > XE_PARCEL_MAX)
		return 0;

	if (filter_reply(uid, (void __user *)st->buffer, used))
		mark_remove(pid);
	return 0;
}

static bool xe_read_registered;

static struct kretprobe xe_read_rp = {
	.kp.symbol_name = "binder_thread_read",
	.entry_handler = xe_read_entry,
	.handler = xe_read_ret,
	.data_size = sizeof(struct xe_read_state),
	.maxactive = 64,
};

/* ------------------------------------------------------------------ init --- */

int xechide_binder_init(void)
{
	int installed = 0, ret;

	xe_parcel_buf = vmalloc(XE_PARCEL_MAX);
	if (!xe_parcel_buf) {
		pr_warn("xechide: binder scratch allocation failed\n");
		return -ENOMEM;
	}

	ret = register_kprobe(&xe_txn_kp);
	if (ret) {
		pr_warn("xechide: binder_transaction probe failed: %d\n", ret);
	} else {
		xe_txn_registered = true;
		installed++;
	}

	ret = register_kretprobe(&xe_read_rp);
	if (ret) {
		pr_warn("xechide: binder_thread_read probe failed: %d\n", ret);
	} else {
		xe_read_registered = true;
		installed++;
	}

	if (installed)
		xechide_stat_inc(XE_STAT_BINDER_HOOKS);
	pr_info("xechide: %d/2 binder probes installed\n", installed);
	return installed ? 0 : -ENOSYS;
}

void xechide_binder_exit(void)
{
	if (xe_read_registered) {
		unregister_kretprobe(&xe_read_rp);
		xe_read_registered = false;
	}
	if (xe_txn_registered) {
		unregister_kprobe(&xe_txn_kp);
		xe_txn_registered = false;
	}
	vfree(xe_parcel_buf);
	xe_parcel_buf = NULL;
}
