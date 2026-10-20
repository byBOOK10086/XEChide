/* SPDX-License-Identifier: GPL-2.0 */
/*
 * XEChide userspace module - shared state, libc subset and helpers.
 *
 * This .so is compiled freestanding (no libc++/libc link): the handful of
 * libc symbols below are resolved from the host process at load time, the
 * string functions are implemented in xechide_util.cpp, and the ABI header
 * from kernel/ is shared verbatim with the kernel module and the host tests.
 */
#ifndef XECHIDE_MOD_H
#define XECHIDE_MOD_H

#include <stdint.h>
#include <stddef.h>

/* The core ABI / pure helpers are shared with the kernel part. */
extern "C" {
#include "xechide.h"
}

#include "zygisk_api.h"

/* ------------------------------------------------------------------ libc --- */

extern "C" {
typedef struct __sFILE FILE;
typedef struct DIR DIR;
struct stat;
struct dirent;

int open(const char *path, int flags, ...);
int open64(const char *path, int flags, ...);
int openat(int dirfd, const char *path, int flags, ...);
int openat64(int dirfd, const char *path, int flags, ...);
int close(int fd);
long read(int fd, void *buf, size_t n);
long write(int fd, const void *buf, size_t n);
int stat(const char *path, struct stat *st);
int lstat(const char *path, struct stat *st);
int access(const char *path, int mode);
int faccessat(int dirfd, const char *path, int mode, int flags);
DIR *opendir(const char *name);
struct dirent *readdir(DIR *dir);
struct dirent *readdir64(DIR *dir);
int closedir(DIR *dir);
FILE *fopen(const char *path, const char *mode);
FILE *fopen64(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
long readlink(const char *path, char *buf, size_t n);
long readlinkat(int dirfd, const char *path, char *buf, size_t n);
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);
int memfd_create(const char *name, unsigned int flags);
int getuid(void);
int getpid(void);
int *__errno(void);
long lseek(int fd, long off, int whence);

/* Implemented in xechide_util.cpp (the module links with -nostdlib). */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
}

/* bionic LP64 layout; only d_name is read. */
struct dirent {
	uint64_t d_ino;
	int64_t d_off;
	unsigned short d_reclen;
	unsigned char d_type;
	char d_name[256];
};

#define XE_ENOENT 2
#define XE_EINTR 4

#define XE_AT_FDCWD (-100)

#define XE_O_RDONLY 0
#define XE_O_CLOEXEC 0x80000

#define XE_PROT_READ 1
#define XE_PROT_WRITE 2
#define XE_MAP_PRIVATE 2
#define XE_MAP_ANONYMOUS 0x20
#define XE_SEEK_SET 0

#define XE_MFD_CLOEXEC 1u

#define XE_CONFIG_PATH "/data/adb/xechide/config"

/* Companion protocol, 16-byte request / 16-byte response headers. */
#define XE_PROTO_MAGIC 0x31434858u /* "XHC1" */
#define XE_PROTO_GET_CONFIG 1u
#define XE_PROTO_ABI 1u
#define XE_PROTO_OK 0u
#define XE_PROTO_NO_FILE 1u
#define XE_PROTO_BAD 2u

/* ----------------------------------------------------------------- state --- */

struct xe_state {
	uint8_t loaded;   /* config read from the root companion */
	uint8_t active;   /* this uid has at least one rule */
	uint8_t self_hide; /* XECHIDE_CF_HIDE_SELF */
	uint32_t uid;
	uint32_t n_rules;
	uint32_t n_binder_names;
	uint32_t n_path_rules;
	struct xechide_config cfg;
	const char *binder_names[16];
	const struct xechide_rule *path_rules[XECHIDE_MAX_RULES];
	uintptr_t self_lo; /* own ELF mapping range, for maps/smaps filtering */
	uintptr_t self_hi;
};

extern struct xe_state g_xe;
extern zg::api_table *g_api;

/* ------------------------------------------------------------------ util --- */

bool xe_read_full(int fd, void *buf, size_t n);
bool xe_write_full(int fd, const void *buf, size_t n);
int xe_read_file(const char *path, void *buf, size_t max);
bool xe_find_lib(const char *name, char *out, size_t outlen);
bool xe_stat_ident(const char *path, unsigned long *dev, unsigned long *ino);

bool xe_fetch_config(void);
void xe_pick_rules(void);
void xe_self_init(void);

bool xe_path_hidden(const char *path);
bool xe_dirent_hidden(int dir_class, const char *name, int namelen);
int xe_dir_class(const char *path);
bool xe_self_map_range(uint64_t start, uint64_t end);

/* ---------------------------------------------------------------- hooks --- */

bool xe_install_hooks(void);
void xe_binder_register(unsigned long dev, unsigned long ino);
void xe_file_register(unsigned long dev, unsigned long ino);

#endif /* XECHIDE_MOD_H */
