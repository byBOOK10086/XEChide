/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Linker script copied from KernelPatch's kpms/demo-hello/hello.lds: it only
 * reserves the NOLOAD placeholder sections the KPM loader expects and leaves
 * every real section intact.
 */
SECTIONS {
	.plt (NOLOAD) : { BYTE(0) }
	.init.plt (NOLOAD) : { BYTE(0) }
	.text.ftrace_trampoline (NOLOAD) : { BYTE(0) }
}
