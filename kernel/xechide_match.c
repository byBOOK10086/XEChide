/* SPDX-License-Identifier: GPL-2.0 */
/*
 * XEChide - pure path/name matching. No kernel API is used here so the same
 * code is compiled into the module and into the host test suite
 * (tests/host). Keep it allocation-free: every hook that calls into this file
 * runs in atomic context.
 */
#include "xechide.h"

#ifdef XECHIDE_HOST
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#else
#include <linux/string.h>
#include <linux/types.h>
#endif

/* Directory classes live in xechide.h (XE_DIR_*). */

static inline bool pkg_equal(const char *s, const char *pkg, size_t n)
{
	return strncmp(s, pkg, n) == 0;
}

/* path starts with prefix and the next byte terminates a component */
static inline bool component_prefix(const char *path, const char *prefix, const char *pkg)
{
	size_t n = strlen(prefix);
	size_t m = strlen(pkg);

	if (strncmp(path, prefix, n) != 0)
		return false;
	path += n;
	if (!pkg_equal(path, pkg, m))
		return false;
	return path[m] == '\0' || path[m] == '/';
}

/* "/data/user/<n>/<pkg>" and "/data/user_de/<n>/<pkg>" */
static bool match_user_dir(const char *path, const char *pkg)
{
	static const char *const prefixes[] = { "/data/user/", "/data/user_de/" };
	unsigned int i;

	for (i = 0; i < 2; i++) {
		const char *p = path;
		size_t n = strlen(prefixes[i]);

		if (strncmp(p, prefixes[i], n) != 0)
			continue;
		p += n;
		if (*p < '0' || *p > '9')
			continue;
		while (*p >= '0' && *p <= '9')
			p++;
		if (*p != '/')
			continue;
		p++;
		if (pkg_equal(p, pkg, strlen(pkg)) &&
		    (p[strlen(pkg)] == '\0' || p[strlen(pkg)] == '/'))
			return true;
	}
	return false;
}

/* "/data/media/<n>/Android/data/<pkg>", "/storage/emulated/<n>/Android/data/<pkg>",
 * "/sdcard/Android/data/<pkg>" */
static bool match_media_dir(const char *path, const char *pkg)
{
	static const char *const roots[] = {
		"/data/media/",
		"/storage/emulated/",
		"/mnt/user/",
		"/sdcard/",
	};
	unsigned int i;

	for (i = 0; i < 4; i++) {
		const char *p = path;
		size_t n = strlen(roots[i]);

		if (strncmp(p, roots[i], n) != 0)
			continue;
		p += n;
		if (i == 3) {
			/* /sdcard/Android/data/<pkg> */
			if (strncmp(p, "Android/data/", 13) != 0)
				continue;
			p += 13;
		} else {
			while (*p >= '0' && *p <= '9')
				p++;
			if (strncmp(p, "/Android/data/", 14) != 0)
				continue;
			p += 14;
		}
		if (pkg_equal(p, pkg, strlen(pkg)) &&
		    (p[strlen(pkg)] == '\0' || p[strlen(pkg)] == '/'))
			return true;
	}
	return false;
}

/*
 * /data/app/<pkg>-<suffix>/...  or  /data/app/~~<hash>/<pkg>-<suffix>/...
 * The suffix is not stable across installs, so only "<pkg>-" is compared.
 */
static bool match_apk_dir(const char *path, const char *pkg)
{
	const char *p;
	size_t m = strlen(pkg);

	if (strncmp(path, "/data/app/", 10) != 0)
		return false;
	p = path + 10;
	if (p[0] == '~' && p[1] == '~') {
		/* skip the "~~<hash>" component */
		while (*p && *p != '/')
			p++;
		if (*p != '/')
			return false;
		p++;
	}
	if (!pkg_equal(p, pkg, m))
		return false;
	return p[m] == '-';
}

/*
 * Package-name databases under /data/system. A detector that can read any of
 * them bypasses the binder filter entirely, so a target uid is denied the
 * whole file (or the whole state directory). Only target uids ever reach this
 * code, so nothing the system needs is affected.
 */
static bool path_prefix_component(const char *path, const char *prefix)
{
	size_t n = strlen(prefix);

	if (strncmp(path, prefix, n) != 0)
		return false;
	return path[n] == '\0' || path[n] == '/';
}

static bool match_system_db(const char *path)
{
	static const char *const files[] = {
		"/data/system/packages.list",
		"/data/system/packages.xml",
		"/data/system/packages-backup.xml",
		"/data/system/appops.xml",
		"/data/system/notification_policy.xml",
		"/data/system/package-usage.list",
	};
	unsigned int i;

	for (i = 0; i < sizeof(files) / sizeof(files[0]); i++)
		if (!strcmp(path, files[i]))
			return true;

	if (path_prefix_component(path, "/data/system/shortcut_service") ||
	    path_prefix_component(path, "/data/system/usagestats") ||
	    path_prefix_component(path, "/data/system/job"))
		return true;

	/* /data/system/users/<n>/package-restrictions.xml and friends */
	if (strncmp(path, "/data/system/users/", 19) == 0) {
		const char *p = path + 19;
		const char *rest;

		while (*p >= '0' && *p <= '9')
			p++;
		if (*p != '/')
			return false;
		rest = p + 1;
		if (!strcmp(rest, "package-restrictions.xml") || !strcmp(rest, "package-usage.list") ||
		    !strcmp(rest, "package.xml"))
			return true;
	}
	return false;
}

int xe_match_path(const struct xechide_rule *rule, const char *path)
{
	const char *pkg = (const char *)rule->pkg;

	if (!path || path[0] != '/')
		return false;
	if (!(rule->flags & XECHIDE_RF_PATHS))
		return false;

	if (match_system_db(path))
		return true;

	if (match_apk_dir(path, pkg))
		return true;
	if (component_prefix(path, "/data/data/", pkg))
		return true;
	if (match_user_dir(path, pkg))
		return true;
	if (match_media_dir(path, pkg))
		return true;
	return false;
}

/* readdir: the name is only one component, so compare it exactly. */
int xe_match_dirent(const struct xechide_rule *rule, unsigned int dir, const char *name, int namelen)
{
	const char *pkg = (const char *)rule->pkg;
	size_t n = namelen < 0 ? strlen(name) : (size_t)namelen;

	if (!(rule->flags & XECHIDE_RF_PATHS))
		return false;

	switch (dir) {
	case XE_DIR_APP:
		/* "<pkg>-<random>" */
		return n > strlen(pkg) && pkg_equal(name, pkg, strlen(pkg)) && name[strlen(pkg)] == '-';
	case XE_DIR_DATA:
	case XE_DIR_MEDIA:
		return n == strlen(pkg) && pkg_equal(name, pkg, n);
	default:
		return false;
	}
}
