/* SPDX-License-Identifier: GPL-2.0 */
/*
 * XEChide userspace module - Zygisk entry points and root companion.
 *
 * Flow:
 *   preAppSpecialize (still with zygote's privileges, before the app sandbox)
 *     -> connect to our root companion, fetch /data/adb/xechide/config
 *     -> select the rules that name this future uid
 *     -> register PLT hooks for libbinder.so (binder replies) and
 *        libjavacore.so / libandroid_runtime.so (file and /proc surface)
 *     -> pltHookCommit()
 *   postAppSpecialize (inside the app sandbox)
 *     -> verify the real uid, locate our own ELF image and erase the two
 *        exported zygisk entry names from .dynstr (anti-fingerprint)
 *
 * The companion runs as root in zygiskd and only serves the config file; the
 * app process never touches /data/adb directly, so no new SELinux policy is
 * needed and no avc denial is generated.
 *
 * If the config cannot be fetched or the uid has no rules, nothing is hooked
 * at all: a process without rules carries no filtering code path that could
 * change its behaviour.
 */
#include "xechide_mod.h"

namespace {

/* One request per connection: magiskd gives each handler invocation its own
 * client socket and closes it after we return. */
unsigned char g_cfg_scratch[sizeof(struct xechide_config) + 1];

void companion_serve(int fd)
{
	uint32_t req[4];
	uint32_t hdr[4];

	if (!xe_read_full(fd, req, sizeof(req)))
		return;
	if (req[0] != XE_PROTO_MAGIC || req[1] != XE_PROTO_GET_CONFIG)
		return;

	hdr[0] = XE_PROTO_MAGIC;
	hdr[1] = XE_PROTO_NO_FILE;
	hdr[2] = 0;
	hdr[3] = XE_PROTO_ABI;

	{
		int n = xe_read_file(XE_CONFIG_PATH, g_cfg_scratch, sizeof(g_cfg_scratch));

		if (n == (int)sizeof(struct xechide_config)) {
			struct xechide_config *cfg = (struct xechide_config *)g_cfg_scratch;

			if (cfg->magic == XECHIDE_ABI_MAGIC &&
			    cfg->version == XECHIDE_ABI_VERSION &&
			    cfg->n_rules <= XECHIDE_MAX_RULES) {
				hdr[1] = XE_PROTO_OK;
				hdr[2] = (uint32_t)n;
			} else {
				hdr[1] = XE_PROTO_BAD;
			}
		} else if (n > 0) {
			hdr[1] = XE_PROTO_BAD;
		}
	}

	if (!xe_write_full(fd, hdr, sizeof(hdr)))
		return;
	if (hdr[2])
		xe_write_full(fd, g_cfg_scratch, hdr[2]);
}

void pre_app(void *impl, void *args);
void post_app(void *impl, const void *args);
void pre_server(void *impl, void *args);
void post_server(void *impl, const void *args);

/* API v4: the loader fills the dev_t/ino_t PLT hook entry points. */
zg::module_abi g_abi = { 4, 0, pre_app, post_app, pre_server, post_server };

} // namespace

bool xe_install_hooks(void)
{
	char path[256];
	unsigned long dev = 0, ino = 0;
	static const char *const file_libs[] = { "libjavacore.so", "libandroid_runtime.so" };

	if (!g_api || !g_api->pltHookRegister)
		return false;

	if (xe_find_lib("libbinder.so", path, sizeof(path)) &&
	    xe_stat_ident(path, &dev, &ino))
		xe_binder_register(dev, ino);

	for (unsigned int i = 0; i < sizeof(file_libs) / sizeof(file_libs[0]); i++) {
		if (xe_find_lib(file_libs[i], path, sizeof(path)) &&
		    xe_stat_ident(path, &dev, &ino))
			xe_file_register(dev, ino);
	}

	if (g_api->pltHookCommit)
		g_api->pltHookCommit();
	return true;
}

namespace {

void pre_app(void *impl, void *args)
{
	zg::AppSpecializeArgs *a;

	(void)impl;
	if (!args || !g_api)
		return;
	a = (zg::AppSpecializeArgs *)args;
	g_xe.uid = (uint32_t)a->uid;
	g_xe.loaded = 0;
	g_xe.active = 0;

	if (!xe_fetch_config())
		return;
	xe_pick_rules();
	if (!g_xe.active)
		return;
	if (!xe_install_hooks())
		g_xe.active = 0;
}

void post_app(void *impl, const void *args)
{
	(void)impl;
	(void)args;
	if (!g_xe.active)
		return;
	if ((uint32_t)getuid() != g_xe.uid) {
		g_xe.active = 0;
		return;
	}
	xe_self_init();
}

void pre_server(void *impl, void *args)
{
	(void)impl;
	(void)args;
}

void post_server(void *impl, const void *args)
{
	(void)impl;
	(void)args;
}

} // namespace

extern "C" __attribute__((visibility("default"))) void zygisk_module_entry(zg::api_table *table,
									 void *env)
{
	(void)env;
	if (!table || !table->registerModule)
		return;
	g_abi.impl = (void *)&g_abi;
	if (!table->registerModule(table, &g_abi))
		return;
	g_api = table;
}

extern "C" __attribute__((visibility("default"))) void zygisk_companion_entry(int fd)
{
	companion_serve(fd);
}
