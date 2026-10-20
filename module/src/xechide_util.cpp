/* SPDX-License-Identifier: GPL-2.0 */
/*
 * XEChide userspace module - freestanding helpers.
 *
 * Everything here runs inside an application process (or the root companion)
 * without a libc++ runtime. The libc functions declared in xechide_mod.h are
 * resolved lazily from the host process; the string routines are implemented
 * below so the module links with -nostdlib.
 */
#include "xechide_mod.h"

/* ------------------------------------------------------------------ state --- */

struct xe_state g_xe;
zg::api_table *g_api;

/* ------------------------------------------------------------ string impl --- */

extern "C" void *memcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;

	while (n--)
		*d++ = *s++;
	return dst;
}

extern "C" void *memmove(void *dst, const void *src, size_t n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;

	if (d == s || n == 0)
		return dst;
	if (d < s) {
		while (n--)
			*d++ = *s++;
	} else {
		d += n;
		s += n;
		while (n--)
			*--d = *--s;
	}
	return dst;
}

extern "C" void *memset(void *dst, int c, size_t n)
{
	unsigned char *d = (unsigned char *)dst;

	while (n--)
		*d++ = (unsigned char)c;
	return dst;
}

extern "C" int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = (const unsigned char *)a;
	const unsigned char *y = (const unsigned char *)b;

	while (n--) {
		if (*x != *y)
			return (int)*x - (int)*y;
		x++;
		y++;
	}
	return 0;
}

extern "C" size_t strlen(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

extern "C" int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

extern "C" int strncmp(const char *a, const char *b, size_t n)
{
	while (n--) {
		if (*a != *b)
			return (int)(unsigned char)*a - (int)(unsigned char)*b;
		if (!*a)
			return 0;
		a++;
		b++;
	}
	return 0;
}

/* --------------------------------------------------------------- IO helpers --- */

bool xe_read_full(int fd, void *buf, size_t n)
{
	unsigned char *p = (unsigned char *)buf;

	while (n) {
		long r = read(fd, p, n);

		if (r < 0) {
			if (*__errno() == XE_EINTR)
				continue;
			return false;
		}
		if (r == 0)
			return false;
		p += (size_t)r;
		n -= (size_t)r;
	}
	return true;
}

bool xe_write_full(int fd, const void *buf, size_t n)
{
	const unsigned char *p = (const unsigned char *)buf;

	while (n) {
		long r = write(fd, p, n);

		if (r < 0) {
			if (*__errno() == XE_EINTR)
				continue;
			return false;
		}
		if (r == 0)
			return false;
		p += (size_t)r;
		n -= (size_t)r;
	}
	return true;
}

/* Read at most @max bytes; returns the byte count or -1. */
int xe_read_file(const char *path, void *buf, size_t max)
{
	int fd = open(path, XE_O_RDONLY | XE_O_CLOEXEC);
	size_t got = 0;
	int ret = -1;

	if (fd < 0)
		return -1;
	while (got < max) {
		long r = read(fd, (unsigned char *)buf + got, max - got);

		if (r < 0) {
			if (*__errno() == XE_EINTR)
				continue;
			goto out;
		}
		if (r == 0)
			break;
		got += (size_t)r;
	}
	ret = (int)got;
out:
	close(fd);
	return ret;
}

/* st_dev / st_ino of @path; both are the bionic dev_t/ino_t values, which is
 * exactly what lsplt compares against its /proc/self/maps scan. */
bool xe_stat_ident(const char *path, unsigned long *dev, unsigned long *ino)
{
	unsigned char st[256] __attribute__((aligned(8)));
	unsigned long d = 0, i = 0;

	if (stat(path, (struct stat *)st) != 0)
		return false;
	memcpy(&d, st, sizeof(d));
	memcpy(&i, st + sizeof(d), sizeof(i));
	*dev = d;
	*ino = i;
	return d != 0 && i != 0;
}

/* --------------------------------------------------------------- maps scan --- */

#define XE_MAPS_MAX (512u * 1024u)

static void *maps_open(size_t *len)
{
	void *buf = mmap(0, XE_MAPS_MAX, XE_PROT_READ | XE_PROT_WRITE,
			 XE_MAP_PRIVATE | XE_MAP_ANONYMOUS, -1, 0);
	int n;

	if ((uintptr_t)buf == (uintptr_t)-1)
		return 0;
	n = xe_read_file("/proc/self/maps", buf, XE_MAPS_MAX - 1);
	if (n <= 0) {
		munmap(buf, XE_MAPS_MAX);
		return 0;
	}
	((char *)buf)[n] = 0;
	*len = (size_t)n;
	return buf;
}

/* The pathname of one /proc/self/maps line, or NULL. */
static const char *maps_path(const char *line)
{
	int field = 0;
	const char *p = line;

	while (*p) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			return 0;
		if (field < 5) {
			while (*p && *p != ' ' && *p != '\t')
				p++;
			field++;
			continue;
		}
		return p;
	}
	return 0;
}

static bool path_ends_with_lib(const char *path, const char *lib)
{
	size_t a = strlen(path), b = strlen(lib);

	if (a <= b)
		return false;
	if (strcmp(path + a - b, lib) != 0)
		return false;
	return path[a - b - 1] == '/';
}

bool xe_find_lib(const char *name, char *out, size_t outlen)
{
	size_t len = 0;
	void *buf = maps_open(&len);
	char *p;
	bool found = false;

	if (!buf)
		return false;
	p = (char *)buf;
	while (*p) {
		char *eol = p;

		while (*eol && *eol != '\n')
			eol++;
		{
			char saved = *eol;
			const char *path;

			*eol = 0;
			path = maps_path(p);
			if (path && path_ends_with_lib(path, name)) {
				size_t n = strlen(path);

				if (n < outlen) {
					memcpy(out, path, n + 1);
					found = true;
				}
			}
			*eol = saved;
			if (found)
				break;
		}
		if (!*eol)
			break;
		p = eol + 1;
	}
	munmap(buf, XE_MAPS_MAX);
	return found;
}

static bool parse_hex(const char **pp, uint64_t *out)
{
	const char *p = *pp;
	uint64_t v = 0;
	int digits = 0;

	for (;;) {
		unsigned int c = (unsigned char)*p;
		unsigned int d;

		if (c >= '0' && c <= '9')
			d = c - '0';
		else if (c >= 'a' && c <= 'f')
			d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			d = c - 'A' + 10;
		else
			break;
		v = (v << 4) | d;
		p++;
		digits++;
	}
	if (!digits)
		return false;
	*pp = p;
	*out = v;
	return true;
}

/*
 * Parse one ELF64 image. Fill the mapping range and erase the two zygisk entry
 * names from .dynstr so a later dlsym()/symbol-table scan cannot fingerprint
 * the module. The loader resolved the entry point long before this runs.
 */
static void self_parse_elf(uintptr_t base)
{
	const unsigned char *e = (const unsigned char *)base;
	uint64_t phoff, dyn_off = 0;
	uint16_t phentsize, phnum;
	uint64_t hi = 0;
	unsigned int i;

	if (e[0] != 0x7f || e[1] != 'E' || e[2] != 'L' || e[3] != 'F' || e[4] != 2)
		return;
	memcpy(&phoff, e + 0x20, 8);
	memcpy(&phentsize, e + 0x36, 2);
	memcpy(&phnum, e + 0x38, 2);
	if (!phoff || phentsize < 56 || !phnum || phnum > 64)
		return;

	for (i = 0; i < phnum; i++) {
		const unsigned char *ph = e + phoff + (uint64_t)i * phentsize;
		uint32_t type;
		uint64_t vaddr, memsz;

		memcpy(&type, ph + 0x00, 4);
		if (type == 1) { /* PT_LOAD */
			memcpy(&vaddr, ph + 0x10, 8);
			memcpy(&memsz, ph + 0x28, 8);
			if (base + vaddr + memsz > hi)
				hi = base + vaddr + memsz;
		} else if (type == 2) { /* PT_DYNAMIC */
			memcpy(&dyn_off, ph + 0x10, 8);
		}
	}
	if (!hi)
		return;
	g_xe.self_lo = base;
	g_xe.self_hi = (hi + 0xfff) & ~(uintptr_t)0xfff;
	if (!dyn_off)
		return;

	{
		const unsigned char *dyn = (const unsigned char *)(base + dyn_off);
		uint64_t strtab = 0, strsz = 0;
		unsigned int k;

		for (k = 0; k < 256; k++) {
			int64_t tag;
			uint64_t val;

			memcpy(&tag, dyn + k * 16, 8);
			memcpy(&val, dyn + k * 16 + 8, 8);
			if (tag == 0)
				break;
			if (tag == 5) /* DT_STRTAB */
				strtab = val;
			else if (tag == 10) /* DT_STRSZ */
				strsz = val;
		}
		if (!strtab || !strsz || strsz > 0x10000)
			return;

		{
			char *s = (char *)(base + strtab);
			static const char *const names[] = { "zygisk_module_entry",
							     "zygisk_companion_entry" };
			unsigned int ni;

			for (ni = 0; ni < 2; ni++) {
				size_t nl = strlen(names[ni]);
				uint64_t off;

				for (off = 0; off + nl <= strsz; off++) {
					if (memcmp(s + off, names[ni], nl) != 0)
						continue;
					{
						uintptr_t page = (uintptr_t)(s + off) & ~(uintptr_t)0xfff;
						uintptr_t end = (uintptr_t)(s + off + nl);
						size_t span = ((end - page) + 0xfff) & ~(size_t)0xfff;

						if (mprotect((void *)page, span,
							     XE_PROT_READ | XE_PROT_WRITE) == 0) {
							memset(s + off, 0, nl);
							mprotect((void *)page, span, XE_PROT_READ);
						}
					}
				}
			}
		}
	}
}

void xe_self_init(void)
{
	uintptr_t anchor = (uintptr_t)(void *)&xe_self_init;
	size_t len = 0;
	void *buf;
	char *p;

	if (g_xe.self_lo)
		return; /* already initialised */
	buf = maps_open(&len);
	if (!buf)
		return;
	p = (char *)buf;
	while (*p) {
		char *eol = p;

		while (*eol && *eol != '\n')
			eol++;
		{
			char saved = *eol;
			const char *q = p;
			uint64_t start = 0, end = 0, off = 0;

			*eol = 0;
			if (parse_hex(&q, &start) && *q == '-') {
				q++;
				if (parse_hex(&q, &end) && start <= anchor && anchor < end) {
					while (*q == ' ' || *q == '\t')
						q++;
					while (*q && *q != ' ' && *q != '\t') /* perms */
						q++;
					while (*q == ' ' || *q == '\t')
						q++;
					if (parse_hex(&q, &off) && start >= off)
						self_parse_elf((uintptr_t)(start - off));
				}
			}
			*eol = saved;
		}
		if (g_xe.self_lo || !*eol)
			break;
		p = eol + 1;
	}
	munmap(buf, XE_MAPS_MAX);
}

bool xe_self_map_range(uint64_t start, uint64_t end)
{
	if (!g_xe.self_lo || !g_xe.self_hi)
		return false;
	return start < (uint64_t)g_xe.self_hi && end > (uint64_t)g_xe.self_lo;
}

/* ----------------------------------------------------------- config/rules --- */

bool xe_fetch_config(void)
{
	uint32_t req[4];
	uint32_t hdr[4];
	int fd;

	if (!g_api || !g_api->connectCompanion)
		return false;
	fd = g_api->connectCompanion(g_api->impl);
	if (fd < 0)
		return false;

	req[0] = XE_PROTO_MAGIC;
	req[1] = XE_PROTO_GET_CONFIG;
	req[2] = g_xe.uid;
	req[3] = XE_PROTO_ABI;
	if (!xe_write_full(fd, req, sizeof(req)) || !xe_read_full(fd, hdr, sizeof(hdr))) {
		close(fd);
		return false;
	}
	if (hdr[0] != XE_PROTO_MAGIC || hdr[1] != XE_PROTO_OK ||
	    hdr[2] != (uint32_t)sizeof(struct xechide_config)) {
		close(fd);
		return false;
	}
	if (!xe_read_full(fd, &g_xe.cfg, sizeof(g_xe.cfg))) {
		close(fd);
		return false;
	}
	close(fd);

	if (g_xe.cfg.magic != XECHIDE_ABI_MAGIC ||
	    g_xe.cfg.version != XECHIDE_ABI_VERSION ||
	    g_xe.cfg.n_rules > XECHIDE_MAX_RULES)
		return false;
	g_xe.loaded = 1;
	g_xe.n_rules = g_xe.cfg.n_rules;
	return true;
}

/* A rule's pkg must be a NUL-terminated package name inside its 128 bytes. */
static bool rule_pkg_ok(const struct xechide_rule *r)
{
	const char *pkg = (const char *)r->pkg;
	unsigned int i;

	for (i = 0; i < XECHIDE_PKG_LEN; i++) {
		if (!pkg[i])
			return i > 0;
		if (i > 0 && pkg[i] == '.')
			continue;
		if (!((pkg[i] >= 'a' && pkg[i] <= 'z') || (pkg[i] >= 'A' && pkg[i] <= 'Z') ||
		      (pkg[i] >= '0' && pkg[i] <= '9') || pkg[i] == '_'))
			return false;
	}
	return false;
}

void xe_pick_rules(void)
{
	uint32_t i;

	g_xe.n_binder_names = 0;
	g_xe.n_path_rules = 0;
	g_xe.self_hide = 0;
	g_xe.active = 0;
	if (!g_xe.loaded)
		return;

	for (i = 0; i < g_xe.n_rules && i < XECHIDE_MAX_RULES; i++) {
		const struct xechide_rule *r = &g_xe.cfg.rules[i];
		uint32_t flags = r->flags & (XECHIDE_RF_BINDER | XECHIDE_RF_PATHS);

		if (!flags)
			continue;
		if (r->target_uid != XECHIDE_UID_ALL && r->target_uid != g_xe.uid)
			continue;
		if (!rule_pkg_ok(r))
			continue;
		if ((flags & XECHIDE_RF_BINDER) && g_xe.n_binder_names < 16)
			g_xe.binder_names[g_xe.n_binder_names++] = (const char *)r->pkg;
		if ((flags & XECHIDE_RF_PATHS) && g_xe.n_path_rules < XECHIDE_MAX_RULES)
			g_xe.path_rules[g_xe.n_path_rules++] = r;
	}
	g_xe.self_hide = (g_xe.cfg.flags & XECHIDE_CF_HIDE_SELF) != 0;
	g_xe.active = (g_xe.n_binder_names > 0 || g_xe.n_path_rules > 0 || g_xe.self_hide);
}

static bool match_media_extra(const struct xechide_rule *r, const char *path);

bool xe_path_hidden(const char *path)
{
	uint32_t i;

	if (!path || path[0] != '/' || !g_xe.active)
		return false;
	if (g_xe.self_hide) {
		static const char self_dir[] = "/data/adb/xechide";
		static const char mod_dir[] = "/data/adb/modules/xechide";

		if (strncmp(path, self_dir, sizeof(self_dir) - 1) == 0 &&
		    (path[sizeof(self_dir) - 1] == 0 || path[sizeof(self_dir) - 1] == '/'))
			return true;
		if (strncmp(path, mod_dir, sizeof(mod_dir) - 1) == 0 &&
		    (path[sizeof(mod_dir) - 1] == 0 || path[sizeof(mod_dir) - 1] == '/'))
			return true;
	}
	for (i = 0; i < g_xe.n_path_rules; i++) {
		const struct xechide_rule *r = g_xe.path_rules[i];

		if (xe_match_path(r, path) || match_media_extra(r, path))
			return true;
	}
	return false;
}

bool xe_dirent_hidden(int dir_class, const char *name, int namelen)
{
	uint32_t i;

	if (!name || dir_class == XE_DIR_NONE || !g_xe.active)
		return false;
	for (i = 0; i < g_xe.n_path_rules; i++)
		if (xe_match_dirent(g_xe.path_rules[i], (unsigned int)dir_class, name, namelen))
			return true;
	return false;
}

static bool str_ends(const char *s, const char *suffix)
{
	size_t a = strlen(s), b = strlen(suffix);

	return a >= b && strcmp(s + a - b, suffix) == 0;
}

/* The shared core matcher covers Android/data; the module also hides the
 * sibling media/obb directories (which the kernel-side iterate_dir list also
 * sees, but xe_match_path predates). Keep the two lists separate so a future
 * core change can absorb this without changing semantics here. */
static bool match_media_extra(const struct xechide_rule *r, const char *path)
{
	static const char *const roots[] = {
		"/sdcard/",
		"/storage/emulated/",
		"/data/media/",
		"/mnt/user/",
	};
	static const char *const kinds[] = { "Android/media/", "Android/obb/" };
	const char *pkg = (const char *)r->pkg;
	size_t plen = strlen(pkg);
	unsigned int i, k;

	if (!(r->flags & XECHIDE_RF_PATHS) || plen == 0)
		return false;
	for (i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
		size_t rl = strlen(roots[i]);

		if (strncmp(path, roots[i], rl) != 0)
			continue;
		{
			const char *p = path + rl;

			if (i == 0) {
				/* /sdcard/... has no user component */
			} else {
				if (*p < '0' || *p > '9')
					continue;
				while (*p >= '0' && *p <= '9')
					p++;
				if (*p != '/')
					continue;
				p++;
			}
			for (k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
				size_t kl = strlen(kinds[k]);

				if (strncmp(p, kinds[k], kl) != 0)
					continue;
				p += kl;
				if (strncmp(p, pkg, plen) == 0 &&
				    (p[plen] == 0 || p[plen] == '/'))
					return true;
			}
		}
	}
	return false;
}

int xe_dir_class(const char *path)
{
	const char *p;

	if (!path)
		return XE_DIR_NONE;
	if (strcmp(path, "/data/app") == 0)
		return XE_DIR_APP;
	if (strcmp(path, "/data/data") == 0)
		return XE_DIR_DATA;
	if (strcmp(path, "/proc") == 0)
		return XE_DIR_PROC;
	if (strncmp(path, "/data/user/", 11) == 0 || strncmp(path, "/data/user_de/", 14) == 0) {
		p = path + (path[10] == '/' ? 11 : 14);
		if (*p < '0' || *p > '9')
			return XE_DIR_NONE;
		while (*p >= '0' && *p <= '9')
			p++;
		return *p == 0 ? XE_DIR_DATA : XE_DIR_NONE;
	}
	if (str_ends(path, "/Android/data") || str_ends(path, "/Android/media") ||
	    str_ends(path, "/Android/obb"))
		return XE_DIR_MEDIA;
	return XE_DIR_NONE;
}
