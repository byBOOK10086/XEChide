/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * KernelPatch's KPM SDK does not ship <linux/types.h>; its type definitions
 * live in <ktypes.h>. The shared XEChide sources include <linux/types.h> for
 * the normal kernel build, so the KPM build gets this one-line shim first on
 * the include path.
 */
#ifndef XECHIDE_KPM_LINUX_TYPES_H
#define XECHIDE_KPM_LINUX_TYPES_H

#include <ktypes.h>

#endif
