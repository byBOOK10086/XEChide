/* SPDX-License-Identifier: GPL-2.0 */
/*
 * XEChide - minimal Zygisk module ABI (API v4).
 *
 * Zygisk is the injection framework shipped with Magisk (and provided by
 * Zygisk Next on top of KernelSU/APatch). The official public header is
 * "zygisk.hpp" from topjohnwu/zygisk-module-sample; its licence is a
 * permissive ISC-style grant. We only need the v4 ABI (PLT hooks by
 * dev_t/inode), which Magisk >= 26.4, Zygisk Next and APatch's Zygisk all
 * implement. Keeping our own copy of the six relevant structs avoids pulling
 * libc++/jni.h into a freestanding .so and pins the exact ABI we target.
 *
 * The layouts below are the load-bearing part:
 *
 *   module_abi  = { long api_version, void *impl, pre, post, preServer, post }
 *   api_table   = { impl, registerModule, hookJniNativeMethods,
 *                   pltHookRegister(dev, ino, sym, fn, backup), exemptFd,
 *                   pltHookCommit, connectCompanion, setOption,
 *                   getModuleDir, getFlags }
 *
 * api_version 4 makes an older Zygisk reject the module at register time
 * instead of filling a shorter table, which is exactly the fail-closed
 * behaviour we want.
 */
#ifndef XECHIDE_ZYGISK_API_H
#define XECHIDE_ZYGISK_API_H

#include <stdint.h>

namespace zg {

/* AppSpecializeArgs is passed by the loader as a v5 struct; the first ten
 * fields are the v1/v3 references and are identical across versions. We only
 * read uid; the trailing optional fields are intentionally not declared. */
struct AppSpecializeArgs {
	int32_t &uid;
	int32_t &gid;
	void *&gids;
	int32_t &runtime_flags;
	void *&rlimits;
	int32_t &mount_external;
	void *&se_info;
	void *&nice_name;
	void *&instruction_set;
	void *&app_data_dir;
};

struct module_abi {
	long api_version;
	void *impl;
	void (*preAppSpecialize)(void *, void *);
	void (*postAppSpecialize)(void *, const void *);
	void (*preServerSpecialize)(void *, void *);
	void (*postServerSpecialize)(void *, const void *);
};

struct api_table {
	void *impl;
	bool (*registerModule)(api_table *, module_abi *);
	void (*hookJniNativeMethods)(void *env, const char *cls, void *methods, int n);
	void (*pltHookRegister)(unsigned long dev, unsigned long ino, const char *symbol, void *fn,
				void **backup);
	bool (*exemptFd)(int fd);
	bool (*pltHookCommit)();
	int (*connectCompanion)(void *impl);
	void (*setOption)(void *impl, int opt);
	int (*getModuleDir)(void *impl);
	uint32_t (*getFlags)(void *impl);
};

} // namespace zg

extern "C" {

/* Entry points resolved with dlsym() by the Zygisk loader. */
__attribute__((visibility("default"))) void zygisk_module_entry(zg::api_table *table, void *env);

__attribute__((visibility("default"))) void zygisk_companion_entry(int fd);

} // extern "C"

#endif /* XECHIDE_ZYGISK_API_H */
