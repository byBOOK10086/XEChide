/* SPDX-License-Identifier: GPL-3.0-only  */
/*
 * Minimal <string.h> for the shared core sources when they are compiled into
 * the userspace module with -DXECHIDE_HOST. The actual implementations live in
 * module/src/xechide_util.cpp; this header only supplies prototypes.
 */
#ifndef XECHIDE_COMPAT_STRING_H
#define XECHIDE_COMPAT_STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* XECHIDE_COMPAT_STRING_H */
