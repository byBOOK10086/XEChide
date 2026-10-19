/* SPDX-License-Identifier: GPL-2.0 */
/*
 * xechide-ctl - push/read the XEChide configuration.
 *
 * This is the only process that talks to the kernel, and it must run as root:
 * it borrows a KernelSU driver descriptor through the legacy reboot supercall,
 * then issues the three XEChide ioctls. It has no crypto and no persistent
 * state - the manager owns both (Android Keystore + its private directory).
 *
 *   xechide-ctl set     < config.bin     (struct xechide_config on stdin)
 *   xechide-ctl get     > config.bin
 *   xechide-ctl stats
 *   xechide-ctl version
 */
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "../kernel/xechide.h"

/* KernelSU installs a driver descriptor for a process that asks with these
 * magic values; the fourth syscall argument receives the fd. The syscall itself
 * fails with EINVAL, which is expected. */
#define KSU_INSTALL_MAGIC1 0xDEADBEEF
#define KSU_INSTALL_MAGIC2 0xCAFEBABE

#define KSU_IOCTL_XECHIDE_GET _IOC(_IOC_READ, 'K', 0xE0, 0)
#define KSU_IOCTL_XECHIDE_SET _IOC(_IOC_WRITE, 'K', 0xE1, 0)
#define KSU_IOCTL_XECHIDE_STATS _IOC(_IOC_READ, 'K', 0xE2, 0)

static struct xechide_config cfg;
static struct xechide_stats stats;

static int install_driver_fd(void)
{
	int fd = -1;

	/* The kernel writes the descriptor number through the fourth argument. */
	syscall(SYS_reboot, KSU_INSTALL_MAGIC1, KSU_INSTALL_MAGIC2, 0, &fd);
	if (fd >= 0)
		return fd;

	fprintf(stderr, "xechide-ctl: no KernelSU driver (is this a KernelSU kernel "
			"with the XEChide patch?)\n");
	return -1;
}

static int read_all(int fd, void *buf, size_t len)
{
	unsigned char *p = buf;
	size_t got = 0;

	while (got < len) {
		ssize_t n = read(fd, p + got, len - got);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			break;
		got += (size_t)n;
	}
	if (got != len) {
		errno = EINVAL;
		return -1;
	}
	/* Anything after the struct means the caller and the kernel disagree on
	 * the ABI: refuse instead of silently ignoring bytes. */
	{
		unsigned char extra;

		if (read(fd, &extra, 1) > 0) {
			errno = E2BIG;
			return -1;
		}
	}
	return 0;
}

static int write_all(int fd, const void *buf, size_t len)
{
	const unsigned char *p = buf;
	size_t put = 0;

	while (put < len) {
		ssize_t n = write(fd, p + put, len - put);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		put += (size_t)n;
	}
	return 0;
}

static int check_header(void)
{
	if (cfg.magic != XECHIDE_ABI_MAGIC) {
		fprintf(stderr, "xechide-ctl: bad magic 0x%08x (expected 0x%08x)\n", cfg.magic,
			XECHIDE_ABI_MAGIC);
		return -1;
	}
	if (cfg.version != XECHIDE_ABI_VERSION) {
		fprintf(stderr, "xechide-ctl: ABI version %u, this build speaks %u\n", cfg.version,
			XECHIDE_ABI_VERSION);
		return -1;
	}
	if (cfg.n_rules > XECHIDE_MAX_RULES) {
		fprintf(stderr, "xechide-ctl: %u rules, max %u\n", cfg.n_rules, XECHIDE_MAX_RULES);
		return -1;
	}
	return 0;
}

static int cmd_set(int fd)
{
	if (read_all(STDIN_FILENO, &cfg, sizeof(cfg)) < 0) {
		fprintf(stderr, "xechide-ctl: cannot read a full config (%zu bytes) from stdin: %s\n",
			sizeof(cfg), strerror(errno));
		return 3;
	}
	if (check_header() < 0)
		return 4;

	if (ioctl(fd, KSU_IOCTL_XECHIDE_SET, &cfg) < 0) {
		fprintf(stderr, "xechide-ctl: SET failed: %s\n", strerror(errno));
		return 5;
	}
	printf("ok: %u rule(s), flags 0x%x\n", cfg.n_rules, cfg.flags);
	return 0;
}

static int cmd_get(int fd)
{
	if (ioctl(fd, KSU_IOCTL_XECHIDE_GET, &cfg) < 0) {
		fprintf(stderr, "xechide-ctl: GET failed: %s\n", strerror(errno));
		return 5;
	}
	if (write_all(STDOUT_FILENO, &cfg, sizeof(cfg)) < 0) {
		fprintf(stderr, "xechide-ctl: cannot write to stdout: %s\n", strerror(errno));
		return 3;
	}
	return 0;
}

static int cmd_stats(int fd)
{
	if (ioctl(fd, KSU_IOCTL_XECHIDE_STATS, &stats) < 0) {
		fprintf(stderr, "xechide-ctl: STATS failed: %s\n", strerror(errno));
		return 5;
	}
	printf("path_hits        %u\n", stats.path_hits);
	printf("dirent_hits      %u\n", stats.dirent_hits);
	printf("proc_hits        %u\n", stats.proc_hits);
	printf("parcel_rebuilt   %u\n", stats.parcel_rebuilt);
	printf("parcel_rewritten %u\n", stats.parcel_rewritten);
	printf("parcel_skipped   %u\n", stats.parcel_skipped);
	printf("binder_errors    %u\n", stats.binder_errors);
	printf("lsm_hooks        %u\n", stats.lsm_hooks);
	printf("binder_hooks     %u\n", stats.binder_hooks);
	return 0;
}

static void usage(FILE *out, const char *argv0)
{
	fprintf(out,
		"usage: %s <command>\n"
		"  set     read one struct xechide_config (%zu bytes) from stdin and apply it\n"
		"  get     write the live configuration to stdout\n"
		"  stats   print the kernel counters\n"
		"  version print this helper's ABI version\n"
		"must run as root\n",
		argv0, sizeof(struct xechide_config));
}

int main(int argc, char **argv)
{
	bool want_help, known;
	int fd, ret;

	if (argc != 2) {
		usage(stderr, argv[0]);
		return 2;
	}

	want_help = !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "help");
	if (want_help) {
		usage(stdout, argv[0]);
		return 0;
	}
	if (!strcmp(argv[1], "version")) {
		printf("xechide-ctl abi %u, config %zu bytes, max %u rules\n", XECHIDE_ABI_VERSION,
		       sizeof(struct xechide_config), XECHIDE_MAX_RULES);
		return 0;
	}

	/* Validate the command before touching the kernel: a typo must not probe
	 * for a driver descriptor. */
	known = !strcmp(argv[1], "set") || !strcmp(argv[1], "get") || !strcmp(argv[1], "stats");
	if (!known) {
		usage(stderr, argv[0]);
		return 2;
	}

	fd = install_driver_fd();
	if (fd < 0)
		return 1;

	if (!strcmp(argv[1], "set"))
		ret = cmd_set(fd);
	else if (!strcmp(argv[1], "get"))
		ret = cmd_get(fd);
	else
		ret = cmd_stats(fd);
	close(fd);
	return ret;
}
