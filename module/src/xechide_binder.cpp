/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * XEChide userspace module - binder reply filtering (L1 in the module form).
 *
 * Zygisk lets us hook the PLT of libbinder.so, so every BINDER_WRITE_READ
 * ioctl is seen with the exact buffer the kernel produced. That is the same
 * data the kernel kretprobe saw in the kernel-mode variant, which is why the
 * tested parcel filter (kernel/xechide_parcel.c) is reused verbatim: the
 * reply Parcel is rewritten in place before libbinder hands it to Java.
 *
 * The read buffer is walked with the public binder command sizes only. Any
 * shape we do not fully understand stops the walk, and any failed validation
 * leaves the parcel byte-identical (fail-open). The filter's scratch arrays
 * are static, so calls are serialised with a spin lock: binder replies are
 * parsed on several threads at once in any normal app.
 */
#include "xechide_mod.h"

namespace {

/* Public uapi layouts (include/uapi/linux/android/binder.h). binder_size_t is
 * __u64 on every ABI, so these structs are 48 / 64 bytes on arm64 and arm. */
struct binder_write_read {
	uint64_t write_size;
	uint64_t write_consumed;
	uint64_t write_buffer;
	uint64_t read_size;
	uint64_t read_consumed;
	uint64_t read_buffer;
};

struct binder_transaction_data {
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

struct binder_transaction_data_secctx {
	struct binder_transaction_data transaction_data;
	uint64_t secctx;
};

static_assert(sizeof(struct binder_write_read) == 48, "binder ABI");
static_assert(sizeof(struct binder_transaction_data) == 64, "binder ABI");
static_assert(sizeof(struct binder_transaction_data_secctx) == 72, "binder ABI");

constexpr uint32_t ioc(unsigned int dir, unsigned int type, unsigned int nr, unsigned int size)
{
	return (dir << 30) | (size << 16) | (type << 8) | nr;
}

constexpr uint32_t XE_BINDER_WRITE_READ = ioc(3, 'b', 1, sizeof(struct binder_write_read));

constexpr uint32_t BR_ERROR = ioc(2, 'r', 0, 4);
constexpr uint32_t BR_OK = ioc(0, 'r', 1, 0);
constexpr uint32_t BR_TRANSACTION_SEC_CTX = ioc(2, 'r', 2, sizeof(struct binder_transaction_data_secctx));
constexpr uint32_t BR_TRANSACTION = ioc(2, 'r', 2, sizeof(struct binder_transaction_data));
constexpr uint32_t BR_REPLY = ioc(2, 'r', 3, sizeof(struct binder_transaction_data));
constexpr uint32_t BR_ACQUIRE_RESULT = ioc(2, 'r', 4, 4);
constexpr uint32_t BR_DEAD_REPLY = ioc(0, 'r', 5, 0);
constexpr uint32_t BR_TRANSACTION_COMPLETE = ioc(0, 'r', 6, 0);
constexpr uint32_t BR_INCREFS = ioc(2, 'r', 7, 16);
constexpr uint32_t BR_ACQUIRE = ioc(2, 'r', 8, 16);
constexpr uint32_t BR_RELEASE = ioc(2, 'r', 9, 16);
constexpr uint32_t BR_DECREFS = ioc(2, 'r', 10, 16);
constexpr uint32_t BR_ATTEMPT_ACQUIRE = ioc(2, 'r', 11, 24);
constexpr uint32_t BR_NOOP = ioc(0, 'r', 12, 0);
constexpr uint32_t BR_SPAWN_LOOPER = ioc(0, 'r', 13, 0);
constexpr uint32_t BR_FINISHED = ioc(0, 'r', 14, 0);
constexpr uint32_t BR_DEAD_BINDER = ioc(2, 'r', 15, 8);
constexpr uint32_t BR_CLEAR_DEATH_NOTIFICATION_DONE = ioc(2, 'r', 16, 8);
constexpr uint32_t BR_FAILED_REPLY = ioc(0, 'r', 17, 0);
constexpr uint32_t BR_FROZEN_REPLY = ioc(0, 'r', 18, 0);
constexpr uint32_t BR_ONEWAY_SPAM_SUSPECT = ioc(0, 'r', 19, 0);
constexpr uint32_t BR_TRANSACTION_PENDING_FROZEN = ioc(0, 'r', 20, 0);
constexpr uint32_t BR_FROZEN_BINDER = ioc(2, 'r', 21, 16);
constexpr uint32_t BR_CLEAR_FREEZE_NOTIFICATION_DONE = ioc(2, 'r', 22, 8);

constexpr uint32_t XE_PARCEL_MAX = 256u * 1024u;

int (*g_real_ioctl)(int, unsigned long, void *);
int g_filter_lock;

void filter_lock(void)
{
	while (__atomic_test_and_set(&g_filter_lock, __ATOMIC_ACQUIRE))
		;
}

void filter_unlock(void)
{
	__atomic_clear(&g_filter_lock, __ATOMIC_RELEASE);
}

/* Walk the read buffer until a BR_REPLY is found. Returns true when the parcel
 * was rewritten. Command sizes are the only kernel-private knowledge here. */
bool filter_read_buffer(unsigned char *buf, unsigned int used)
{
	unsigned int off = 0;

	if (g_xe.n_binder_names == 0)
		return false;

	while (off + 4 <= used) {
		uint32_t cmd;

		memcpy(&cmd, buf + off, 4);

		if (cmd == BR_REPLY) {
			struct binder_transaction_data tr;
			unsigned int mode = 0;
			unsigned int new_len;

			if (off + 4 + sizeof(tr) > used)
				return false;
			memcpy(&tr, buf + off + 4, sizeof(tr));
			if (!tr.data_size || tr.data_size > XE_PARCEL_MAX || tr.offsets_size ||
			    !tr.data.ptr.buffer)
				return false;
			filter_lock();
			new_len = xe_parcel_filter((unsigned char *)(uintptr_t)tr.data.ptr.buffer,
						   (unsigned int)tr.data_size, g_xe.binder_names,
						   (int)g_xe.n_binder_names, &mode);
			filter_unlock();
			if (!new_len || new_len > tr.data_size)
				return false;
			tr.data_size = new_len;
			memcpy(buf + off + 4, &tr, sizeof(tr));
			return true;
		}

		switch (cmd) {
		case BR_OK:
		case BR_DEAD_REPLY:
		case BR_TRANSACTION_COMPLETE:
		case BR_NOOP:
		case BR_SPAWN_LOOPER:
		case BR_FINISHED:
		case BR_FAILED_REPLY:
		case BR_FROZEN_REPLY:
		case BR_ONEWAY_SPAM_SUSPECT:
		case BR_TRANSACTION_PENDING_FROZEN:
			off += 4;
			break;
		case BR_ERROR:
		case BR_ACQUIRE_RESULT:
			off += 8;
			break;
		case BR_INCREFS:
		case BR_ACQUIRE:
		case BR_RELEASE:
		case BR_DECREFS:
			off += 4 + 16;
			break;
		case BR_ATTEMPT_ACQUIRE:
			off += 4 + 24;
			break;
		case BR_DEAD_BINDER:
		case BR_CLEAR_DEATH_NOTIFICATION_DONE:
		case BR_CLEAR_FREEZE_NOTIFICATION_DONE:
			off += 4 + 8;
			break;
		case BR_FROZEN_BINDER:
			off += 4 + 16;
			break;
		case BR_TRANSACTION_SEC_CTX:
			off += 4 + sizeof(struct binder_transaction_data_secctx);
			break;
		case BR_TRANSACTION:
			off += 4 + sizeof(struct binder_transaction_data);
			break;
		default:
			return false;
		}
	}
	return false;
}

} // namespace

extern "C" int xe_hook_ioctl(int fd, unsigned long req, void *arg)
{
	int ret = g_real_ioctl(fd, req, arg);

	if (ret < 0 || !arg || !g_xe.active || req != XE_BINDER_WRITE_READ)
		return ret;
	{
		struct binder_write_read *bwr = (struct binder_write_read *)arg;

		if (bwr->read_consumed && bwr->read_buffer &&
		    bwr->read_consumed <= 4u * 1024u * 1024u)
			filter_read_buffer((unsigned char *)(uintptr_t)bwr->read_buffer,
					   (unsigned int)bwr->read_consumed);
	}
	return ret;
}

void xe_binder_register(unsigned long dev, unsigned long ino)
{
	if (!g_api || !g_api->pltHookRegister)
		return;
	g_api->pltHookRegister(dev, ino, "ioctl", (void *)xe_hook_ioctl, (void **)&g_real_ioctl);
}
