/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * XEChide userspace module - file and /proc surface.
 *
 * The module hooks the PLT of libjavacore.so (java.io native methods) and
 * libandroid_runtime.so, which is where every File/Files/Os call made by a
 * detector app lands. Three jobs are done there:
 *
 *   1. hide external-storage package directories (HMA does not do this at all)
 *      by filtering readdir() results of .../Android/{data,media,obb};
 *   2. forge ENOENT for the hidden app's paths and for our own module/config
 *      paths, so exists()/canRead()/open() all agree with the listing;
 *   3. serve /proc/self/maps and /proc/self/smaps through a memfd with the
 *      module's own mappings removed. The memfd is named "jit-cache" so the
 *      replacement descriptor does not stand out either.
 *
 * Every hook calls the original through the pointer lsplt saved at commit
 * time. If a hook is not installed (symbol missing in that library), the
 * kernel behaviour is simply never intercepted; nothing here can make a
 * normal file operation fail.
 */
#include "xechide_mod.h"

#include <stdarg.h>

namespace {

constexpr int O_CREAT = 0x40;
constexpr int O_TMPFILE = 0x410000;

constexpr unsigned int XE_MAPS_MAX_LINE = 1024;

typedef int (*open_fn)(const char *, int, ...);
typedef int (*openat_fn)(int, const char *, int, ...);
typedef int (*stat_fn)(const char *, struct stat *);
typedef int (*access_fn)(const char *, int);
typedef int (*faccessat_fn)(int, const char *, int, int);
typedef struct DIR *(*opendir_fn)(const char *);
typedef struct dirent *(*readdir_fn)(struct DIR *);
typedef int (*closedir_fn)(struct DIR *);
typedef FILE *(*fopen_fn)(const char *, const char *);
typedef long (*readlink_fn)(const char *, char *, size_t);
typedef long (*readlinkat_fn)(int, const char *, char *, size_t);

open_fn g_open;
open_fn g_open64;
openat_fn g_openat;
openat_fn g_openat64;
stat_fn g_stat;
stat_fn g_lstat;
stat_fn g_stat64;
stat_fn g_lstat64;
access_fn g_access;
faccessat_fn g_faccessat;
opendir_fn g_opendir;
readdir_fn g_readdir;
readdir_fn g_readdir64;
closedir_fn g_closedir;
fopen_fn g_fopen;
fopen_fn g_fopen64;
readlink_fn g_readlink;
readlinkat_fn g_readlinkat;

/* -------------------------------------------------------- maps filtering --- */

bool parse_hex(const char **pp, uint64_t *out)
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

bool line_is_range(const char *line, unsigned int len, uint64_t *start, uint64_t *end)
{
	const char *p = line;
	const char *limit = line + len;
	uint64_t a = 0, b = 0;

	if (!parse_hex(&p, &a) || p >= limit || *p != '-')
		return false;
	p++;
	if (!parse_hex(&p, &b))
		return false;
	*start = a;
	*end = b;
	return true;
}

bool maps_path(const char *path)
{
	if (!path || strncmp(path, "/proc/", 6) != 0)
		return false;
	if (strcmp(path, "/proc/self/maps") == 0 || strcmp(path, "/proc/self/smaps") == 0 ||
	    strcmp(path, "/proc/thread-self/maps") == 0 ||
	    strcmp(path, "/proc/thread-self/smaps") == 0)
		return true;
	{
		const char *p = path + 6;
		int pid = 0;

		if (*p < '0' || *p > '9')
			return false;
		while (*p >= '0' && *p <= '9') {
			pid = pid * 10 + (*p - '0');
			if (pid > 1000000)
				return false;
			p++;
		}
		if (pid != getpid())
			return false;
		return strcmp(p, "/maps") == 0 || strcmp(p, "/smaps") == 0;
	}
}

bool writable_mode(const char *mode)
{
	if (!mode)
		return true;
	for (; *mode; mode++)
		if (*mode == 'w' || *mode == 'a' || *mode == '+')
			return true;
	return false;
}

/* Stream the real file into a memfd, dropping our own VMA lines (and the
 * matching smaps block). Returns an fd positioned at 0, or -1. */
int serve_maps(const char *path)
{
	bool smaps = strcmp(path + strlen(path) - 6, "/smaps") == 0;
	int in = g_open(path, XE_O_RDONLY | XE_O_CLOEXEC, 0);
	int out;
	char rb[4096];
	char line[XE_MAPS_MAX_LINE];
	unsigned int ll = 0;
	bool drop = false;
	int rc = 0;

	if (in < 0)
		return -1;
	out = memfd_create("jit-cache", XE_MFD_CLOEXEC);
	if (out < 0) {
		close(in);
		return -1;
	}

	for (;;) {
		long n = read(in, rb, sizeof(rb));

		if (n < 0) {
			if (*__errno() == XE_EINTR)
				continue;
			rc = -1;
			break;
		}
		if (n == 0)
			break;
		for (long i = 0; i < n; i++) {
			char c = rb[i];

			if (c != '\n') {
				if (ll + 1 < sizeof(line))
					line[ll++] = c;
				continue;
			}
			{
				uint64_t s = 0, e = 0;
				bool addr = line_is_range(line, ll, &s, &e);

				if (smaps) {
					if (addr)
						drop = xe_self_map_range(s, e);
					if (!drop && (!xe_write_full(out, line, ll) ||
						     !xe_write_full(out, "\n", 1))) {
						rc = -1;
						break;
					}
				} else if (!(addr && xe_self_map_range(s, e))) {
					if (!xe_write_full(out, line, ll) ||
					    !xe_write_full(out, "\n", 1)) {
						rc = -1;
						break;
					}
				}
			}
			ll = 0;
		}
		if (rc < 0)
			break;
	}
	if (rc == 0 && ll) {
		uint64_t s = 0, e = 0;
		bool addr = line_is_range(line, ll, &s, &e);
		bool keep;

		if (smaps) {
			if (addr)
				drop = xe_self_map_range(s, e);
			keep = !drop;
		} else {
			keep = !(addr && xe_self_map_range(s, e));
		}
		if (keep && (!xe_write_full(out, line, ll) || !xe_write_full(out, "\n", 1)))
			rc = -1;
	}
	close(in);
	if (rc < 0) {
		close(out);
		return -1;
	}
	lseek(out, 0, XE_SEEK_SET);
	return out;
}

/* /proc/self/map_files/<start>-<end> names our mapping; hide our own ranges. */
bool map_files_hits_self(const char *path)
{
	const char *p = 0;

	if (!path)
		return false;
	if (strncmp(path, "/proc/self/map_files/", 21) == 0)
		p = path + 21;
	else if (strncmp(path, "/proc/thread-self/map_files/", 28) == 0)
		p = path + 28;
	else if (strncmp(path, "/proc/", 6) == 0) {
		const char *q = path + 6;
		int pid = 0;

		if (*q < '0' || *q > '9')
			return false;
		while (*q >= '0' && *q <= '9') {
			pid = pid * 10 + (*q - '0');
			if (pid > 1000000)
				return false;
			q++;
		}
		if (pid != getpid() || strncmp(q, "/map_files/", 11) != 0)
			return false;
		p = q + 11;
	}
	if (!p)
		return false;
	{
		uint64_t a = 0, b = 0;

		if (!parse_hex(&p, &a) || *p != '-')
			return false;
		p++;
		if (!parse_hex(&p, &b))
			return false;
		return xe_self_map_range(a, b);
	}
}

bool hidden_path(const char *path)
{
	return xe_path_hidden(path) || map_files_hits_self(path);
}

/* ---------------------------------------------------------- DIR tracking --- */

struct dir_slot {
	struct DIR *dir;
	unsigned char used;
	unsigned char cls;
	char path[192];
};

dir_slot g_dirs[64];
int g_dir_lock;

void dir_lock(void)
{
	while (__atomic_test_and_set(&g_dir_lock, __ATOMIC_ACQUIRE))
		;
}

void dir_unlock(void)
{
	__atomic_clear(&g_dir_lock, __ATOMIC_RELEASE);
}

void dir_record(struct DIR *d, const char *path)
{
	int cls = xe_dir_class(path);
	int i;

	if (cls == XE_DIR_NONE || !path)
		return;
	dir_lock();
	for (i = 0; i < 64; i++) {
		if (g_dirs[i].used && g_dirs[i].dir == d)
			break;
		if (!g_dirs[i].used) {
			size_t n = strlen(path);

			g_dirs[i].used = 1;
			g_dirs[i].dir = d;
			g_dirs[i].cls = (unsigned char)cls;
			if (n >= sizeof(g_dirs[i].path))
				n = sizeof(g_dirs[i].path) - 1;
			memcpy(g_dirs[i].path, path, n);
			g_dirs[i].path[n] = 0;
			break;
		}
	}
	dir_unlock();
}

int dir_class_of(struct DIR *d)
{
	int cls = XE_DIR_NONE;
	int i;

	dir_lock();
	for (i = 0; i < 64; i++) {
		if (g_dirs[i].used && g_dirs[i].dir == d) {
			cls = g_dirs[i].cls;
			break;
		}
	}
	dir_unlock();
	return cls;
}

void dir_forget(struct DIR *d)
{
	int i;

	dir_lock();
	for (i = 0; i < 64; i++) {
		if (g_dirs[i].used && g_dirs[i].dir == d) {
			g_dirs[i].used = 0;
			break;
		}
	}
	dir_unlock();
}

/* --------------------------------------------------------------- wrappers --- */

int extract_mode(int flags, va_list ap)
{
	int mode = 0;

	if (flags & (O_CREAT | O_TMPFILE))
		mode = va_arg(ap, int);
	return mode;
}

struct dirent *filter_dirent(struct DIR *d, struct dirent *e)
{
	if (!e)
		return 0;
	while (e) {
		int cls = dir_class_of(d);

		if (cls == XE_DIR_NONE || !xe_dirent_hidden(cls, e->d_name, -1))
			return e;
		e = g_readdir ? g_readdir(d) : 0;
	}
	return 0;
}

} // namespace

extern "C" int xe_hook_open(const char *path, int flags, ...)
{
	int mode;
	va_list ap;

	va_start(ap, flags);
	mode = extract_mode(flags, ap);
	va_end(ap);
	if (hidden_path(path)) {
		*__errno() = XE_ENOENT;
		return -1;
	}
	if (maps_path(path)) {
		int fd = serve_maps(path);

		if (fd >= 0)
			return fd;
	}
	return g_open(path, flags, mode);
}

extern "C" int xe_hook_open64(const char *path, int flags, ...)
{
	int mode;
	va_list ap;

	va_start(ap, flags);
	mode = extract_mode(flags, ap);
	va_end(ap);
	if (hidden_path(path)) {
		*__errno() = XE_ENOENT;
		return -1;
	}
	if (maps_path(path)) {
		int fd = serve_maps(path);

		if (fd >= 0)
			return fd;
	}
	return g_open64(path, flags, mode);
}

extern "C" int xe_hook_openat(int dirfd, const char *path, int flags, ...)
{
	int mode;
	va_list ap;

	va_start(ap, flags);
	mode = extract_mode(flags, ap);
	va_end(ap);
	if (path && path[0] == '/' && hidden_path(path)) {
		*__errno() = XE_ENOENT;
		return -1;
	}
	if (path && path[0] == '/' && maps_path(path)) {
		int fd = serve_maps(path);

		if (fd >= 0)
			return fd;
	}
	return g_openat(dirfd, path, flags, mode);
}

extern "C" int xe_hook_openat64(int dirfd, const char *path, int flags, ...)
{
	int mode;
	va_list ap;

	va_start(ap, flags);
	mode = extract_mode(flags, ap);
	va_end(ap);
	if (path && path[0] == '/' && hidden_path(path)) {
		*__errno() = XE_ENOENT;
		return -1;
	}
	if (path && path[0] == '/' && maps_path(path)) {
		int fd = serve_maps(path);

		if (fd >= 0)
			return fd;
	}
	return g_openat64(dirfd, path, flags, mode);
}

extern "C" FILE *xe_hook_fopen(const char *path, const char *mode)
{
	if (hidden_path(path)) {
		*__errno() = XE_ENOENT;
		return 0;
	}
	if (!writable_mode(mode) && maps_path(path)) {
		int fd = serve_maps(path);

		if (fd >= 0) {
			FILE *f = fdopen(fd, "r");

			if (f)
				return f;
			close(fd);
		}
	}
	return g_fopen(path, mode);
}

extern "C" FILE *xe_hook_fopen64(const char *path, const char *mode)
{
	if (hidden_path(path)) {
		*__errno() = XE_ENOENT;
		return 0;
	}
	if (!writable_mode(mode) && maps_path(path)) {
		int fd = serve_maps(path);

		if (fd >= 0) {
			FILE *f = fdopen(fd, "r");

			if (f)
				return f;
			close(fd);
		}
	}
	return g_fopen64(path, mode);
}

extern "C" int xe_hook_stat(const char *path, struct stat *st)
{
	if (hidden_path(path)) {
		*__errno() = XE_ENOENT;
		return -1;
	}
	return g_stat(path, st);
}

extern "C" int xe_hook_lstat(const char *path, struct stat *st)
{
	if (hidden_path(path)) {
		*__errno() = XE_ENOENT;
		return -1;
	}
	return g_lstat(path, st);
}

extern "C" int xe_hook_stat64(const char *path, struct stat *st)
{
	if (hidden_path(path)) {
		*__errno() = XE_ENOENT;
		return -1;
	}
	return g_stat64(path, st);
}

extern "C" int xe_hook_lstat64(const char *path, struct stat *st)
{
	if (hidden_path(path)) {
		*__errno() = XE_ENOENT;
		return -1;
	}
	return g_lstat64(path, st);
}

extern "C" int xe_hook_access(const char *path, int mode)
{
	if (hidden_path(path)) {
		*__errno() = XE_ENOENT;
		return -1;
	}
	return g_access(path, mode);
}

extern "C" int xe_hook_faccessat(int dirfd, const char *path, int mode, int flags)
{
	if (path && path[0] == '/' && hidden_path(path)) {
		*__errno() = XE_ENOENT;
		return -1;
	}
	return g_faccessat(dirfd, path, mode, flags);
}

extern "C" struct DIR *xe_hook_opendir(const char *path)
{
	struct DIR *d;

	if (hidden_path(path)) {
		*__errno() = XE_ENOENT;
		return 0;
	}
	d = g_opendir(path);
	if (d)
		dir_record(d, path);
	return d;
}

extern "C" struct dirent *xe_hook_readdir(struct DIR *d)
{
	return filter_dirent(d, g_readdir(d));
}

extern "C" struct dirent *xe_hook_readdir64(struct DIR *d)
{
	struct dirent *e = g_readdir64 ? g_readdir64(d) : 0;

	if (!e)
		return 0;
	for (;;) {
		int cls = dir_class_of(d);

		if (cls == XE_DIR_NONE || !xe_dirent_hidden(cls, e->d_name, -1))
			return e;
		e = g_readdir64(d);
		if (!e)
			return 0;
	}
}

extern "C" int xe_hook_closedir(struct DIR *d)
{
	dir_forget(d);
	return g_closedir(d);
}

extern "C" long xe_hook_readlink(const char *path, char *buf, size_t n)
{
	if (hidden_path(path)) {
		*__errno() = XE_ENOENT;
		return -1;
	}
	return g_readlink(path, buf, n);
}

extern "C" long xe_hook_readlinkat(int dirfd, const char *path, char *buf, size_t n)
{
	if (path && path[0] == '/' && hidden_path(path)) {
		*__errno() = XE_ENOENT;
		return -1;
	}
	return g_readlinkat(dirfd, path, buf, n);
}

void xe_file_register(unsigned long dev, unsigned long ino)
{
	struct reg {
		const char *sym;
		void *fn;
		void **backup;
	};

	if (!g_api || !g_api->pltHookRegister)
		return;

	const reg table[] = {
		{ "open", (void *)xe_hook_open, (void **)&g_open },
		{ "open64", (void *)xe_hook_open64, (void **)&g_open64 },
		{ "openat", (void *)xe_hook_openat, (void **)&g_openat },
		{ "openat64", (void *)xe_hook_openat64, (void **)&g_openat64 },
		{ "fopen", (void *)xe_hook_fopen, (void **)&g_fopen },
		{ "fopen64", (void *)xe_hook_fopen64, (void **)&g_fopen64 },
		{ "stat", (void *)xe_hook_stat, (void **)&g_stat },
		{ "lstat", (void *)xe_hook_lstat, (void **)&g_lstat },
		{ "stat64", (void *)xe_hook_stat64, (void **)&g_stat64 },
		{ "lstat64", (void *)xe_hook_lstat64, (void **)&g_lstat64 },
		{ "access", (void *)xe_hook_access, (void **)&g_access },
		{ "faccessat", (void *)xe_hook_faccessat, (void **)&g_faccessat },
		{ "opendir", (void *)xe_hook_opendir, (void **)&g_opendir },
		{ "readdir", (void *)xe_hook_readdir, (void **)&g_readdir },
		{ "readdir64", (void *)xe_hook_readdir64, (void **)&g_readdir64 },
		{ "closedir", (void *)xe_hook_closedir, (void **)&g_closedir },
		{ "readlink", (void *)xe_hook_readlink, (void **)&g_readlink },
		{ "readlinkat", (void *)xe_hook_readlinkat, (void **)&g_readlinkat },
	};

	for (unsigned int i = 0; i < sizeof(table) / sizeof(table[0]); i++)
		g_api->pltHookRegister(dev, ino, table[i].sym, table[i].fn, table[i].backup);
}
