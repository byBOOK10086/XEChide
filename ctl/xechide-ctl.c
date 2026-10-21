/* SPDX-License-Identifier: GPL-3.0-only  */
/*
 * xechide-ctl - apply/read the XEChide configuration.
 *
 * The same packed struct (kernel/xechide.h) drives both delivery forms:
 *
 *   module form : written to /data/adb/xechide/config, read by the Zygisk
 *                 module's root companion and handed to app processes;
 *   kernel form : sent to the KernelSU driver with the XECHIDE_SET ioctl
 *                 (the optional advanced build).
 *
 * `apply` does both: it atomically writes the file first and then, if a
 * patched KernelSU driver is present, applies the same bytes to the kernel.
 * The two forms therefore always share one configuration.
 *
 * It has no crypto and no persistent state; the manager owns both. Must run
 * as root (through su), because /data/adb is root-only.
 *
 *   xechide-ctl apply    < config.bin   (module file + kernel when available)
 *   xechide-ctl set      < config.bin   (kernel only)
 *   xechide-ctl get        > config.bin (kernel only)
 *   xechide-ctl file-get   > config.bin (module file only)
 *   xechide-ctl status                  (module/kernel presence, key=value)
 *   xechide-ctl stats                   (kernel counters)
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

#define CONFIG_DIR "/data/adb/xechide"
#define CONFIG_PATH CONFIG_DIR "/config"
#define STATUS_PATH CONFIG_DIR "/status"
#define MODULE_DIR "/data/adb/modules/xechide"
#define MODULE_PROP MODULE_DIR "/module.prop"
#define MODULE_SO MODULE_DIR "/zygisk/arm64-v8a.so"
#define KPM_FILE "/data/adb/ap/kpm/xechide/xechide.kpm"

static struct xechide_config cfg;
static struct xechide_stats stats;

static int install_driver_fd(bool verbose)
{
	int fd = -1;

	/* The kernel writes the descriptor number through the fourth argument. */
	syscall(SYS_reboot, KSU_INSTALL_MAGIC1, KSU_INSTALL_MAGIC2, 0, &fd);
	if (fd >= 0)
		return fd;

	if (verbose)
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

static int read_config_stdin(void)
{
	if (read_all(STDIN_FILENO, &cfg, sizeof(cfg)) < 0) {
		fprintf(stderr, "xechide-ctl: cannot read a full config (%zu bytes) from stdin: %s\n",
			sizeof(cfg), strerror(errno));
		return 3;
	}
	if (cfg.magic != XECHIDE_ABI_MAGIC) {
		fprintf(stderr, "xechide-ctl: bad magic 0x%08x (expected 0x%08x)\n", cfg.magic,
			XECHIDE_ABI_MAGIC);
		return 4;
	}
	if (cfg.version != XECHIDE_ABI_VERSION) {
		fprintf(stderr, "xechide-ctl: ABI version %u, this build speaks %u\n", cfg.version,
			XECHIDE_ABI_VERSION);
		return 4;
	}
	if (cfg.n_rules > XECHIDE_MAX_RULES) {
		fprintf(stderr, "xechide-ctl: %u rules, max %u\n", cfg.n_rules, XECHIDE_MAX_RULES);
		return 4;
	}
	return 0;
}

static int write_config_file(void)
{
	char tmp[sizeof(CONFIG_PATH) + 8];
	int fd;

	if (mkdir(CONFIG_DIR, 0700) < 0 && errno != EEXIST) {
		fprintf(stderr, "xechide-ctl: mkdir %s failed: %s\n", CONFIG_DIR, strerror(errno));
		return 5;
	}
	snprintf(tmp, sizeof(tmp), "%s.tmp", CONFIG_PATH);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		fprintf(stderr, "xechide-ctl: open %s failed: %s\n", tmp, strerror(errno));
		return 5;
	}
	if (write_all(fd, &cfg, sizeof(cfg)) < 0) {
		fprintf(stderr, "xechide-ctl: write %s failed: %s\n", tmp, strerror(errno));
		close(fd);
		unlink(tmp);
		return 5;
	}
	(void)fsync(fd);
	close(fd);
	if (rename(tmp, CONFIG_PATH) < 0) {
		fprintf(stderr, "xechide-ctl: rename to %s failed: %s\n", CONFIG_PATH, strerror(errno));
		unlink(tmp);
		return 5;
	}
	(void)chmod(CONFIG_PATH, 0600);
	return 0;
}

static void mirror_apatch_config(void)
{
	static const char apatch_dir[] = "/data/data/me.bmax.apatch";
	static const char apatch_files[] = "/data/data/me.bmax.apatch/files";
	static const char apatch_cfg[] = "/data/data/me.bmax.apatch/files/xechide.cfg";
	static const char apatch_tmp[] = "/data/data/me.bmax.apatch/files/xechide.cfg.tmp";
	struct stat dirst;
	int fd;

	/* The APatch KPM loader/control runs in the APatch manager app's uid.
	 * That process cannot read /data/adb, so keep a 0600 copy in APatch's
	 * private data. Failure here never fails the primary apply. */
	if (stat(apatch_dir, &dirst) < 0)
		return;
	(void)mkdir(apatch_files, 0700);
	(void)chown(apatch_files, dirst.st_uid, dirst.st_gid);
	(void)chmod(apatch_files, 0700);
	fd = open(apatch_tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return;
	if (write_all(fd, &cfg, sizeof(cfg)) < 0) {
		close(fd);
		unlink(apatch_tmp);
		return;
	}
	(void)fsync(fd);
	close(fd);
	if (rename(apatch_tmp, apatch_cfg) < 0) {
		unlink(apatch_tmp);
		return;
	}
	(void)chown(apatch_cfg, dirst.st_uid, dirst.st_gid);
	(void)chmod(apatch_cfg, 0600);
}

static int cmd_apply(void)
{
	int ret = read_config_stdin();
	int fd;

	if (ret)
		return ret;
	ret = write_config_file();
	if (ret)
		return ret;
	printf("file=ok\n");
	mirror_apatch_config();

	fd = install_driver_fd(false);
	if (fd < 0) {
		printf("kernel=no-driver\n");
		return 0;
	}
	if (ioctl(fd, KSU_IOCTL_XECHIDE_SET, &cfg) < 0) {
		printf("kernel=failed\n");
		fprintf(stderr, "xechide-ctl: kernel SET failed: %s\n", strerror(errno));
		close(fd);
		return 0; /* the module form is already applied */
	}
	close(fd);
	printf("kernel=ok\n");
	return 0;
}

static int cmd_set(void)
{
	int ret = read_config_stdin();
	int fd;

	if (ret)
		return ret;
	fd = install_driver_fd(true);
	if (fd < 0)
		return 1;
	if (ioctl(fd, KSU_IOCTL_XECHIDE_SET, &cfg) < 0) {
		fprintf(stderr, "xechide-ctl: SET failed: %s\n", strerror(errno));
		close(fd);
		return 5;
	}
	close(fd);
	printf("ok: %u rule(s), flags 0x%x\n", cfg.n_rules, cfg.flags);
	return 0;
}

static int cmd_get(void)
{
	int fd = install_driver_fd(true);

	if (fd < 0)
		return 1;
	if (ioctl(fd, KSU_IOCTL_XECHIDE_GET, &cfg) < 0) {
		fprintf(stderr, "xechide-ctl: GET failed: %s\n", strerror(errno));
		close(fd);
		return 5;
	}
	close(fd);
	if (write_all(STDOUT_FILENO, &cfg, sizeof(cfg)) < 0) {
		fprintf(stderr, "xechide-ctl: cannot write to stdout: %s\n", strerror(errno));
		return 3;
	}
	return 0;
}

static int cmd_file_get(void)
{
	unsigned char buf[4096];
	int fd = open(CONFIG_PATH, O_RDONLY);

	if (fd < 0) {
		fprintf(stderr, "xechide-ctl: open %s failed: %s\n", CONFIG_PATH, strerror(errno));
		return 5;
	}
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));

		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "xechide-ctl: read failed: %s\n", strerror(errno));
			close(fd);
			return 5;
		}
		if (n == 0)
			break;
		if (write_all(STDOUT_FILENO, buf, (size_t)n) < 0) {
			fprintf(stderr, "xechide-ctl: cannot write to stdout: %s\n", strerror(errno));
			close(fd);
			return 3;
		}
	}
	close(fd);
	return 0;
}

static int read_small(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY);
	size_t got = 0;

	if (fd < 0)
		return -1;
	while (got + 1 < cap) {
		ssize_t n = read(fd, buf + got, cap - 1 - got);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		got += (size_t)n;
	}
	close(fd);
	buf[got] = 0;
	return (int)got;
}

static bool print_version_from(const char *path)
{
	char buf[2048];
	char *p;

	if (read_small(path, buf, sizeof(buf)) <= 0)
		return false;
	p = strstr(buf, "version=");
	if (!p)
		return false;
	p += strlen("version=");
	{
		char *e = p;

		while (*e && *e != '\n' && *e != '\r')
			e++;
		*e = 0;
		printf("version=%s\n", p);
		return true;
	}
}

static int cmd_status(void)
{
	struct stat st;
	int fd;

	printf("module=%d\n", access(MODULE_PROP, F_OK) == 0 ? 1 : 0);
	printf("module_so=%d\n", access(MODULE_SO, F_OK) == 0 ? 1 : 0);
	printf("kpm=%d\n", access(KPM_FILE, F_OK) == 0 ? 1 : 0);

	if (access(CONFIG_PATH, F_OK) == 0 && stat(CONFIG_PATH, &st) == 0) {
		printf("config=1\n");
		printf("config_bytes=%lld\n", (long long)st.st_size);
		printf("config_mtime=%lld\n", (long long)st.st_mtime);
	} else {
		printf("config=0\n");
		printf("config_bytes=0\n");
	}

	fd = install_driver_fd(false);
	if (fd >= 0) {
		int ok = ioctl(fd, KSU_IOCTL_XECHIDE_GET, &cfg) == 0;

		close(fd);
		printf("kernel=%d\n", ok ? 1 : 0);
	} else {
		printf("kernel=0\n");
	}

	if (!print_version_from(STATUS_PATH))
		(void)print_version_from(MODULE_PROP);
	return 0;
}

static int cmd_stats(void)
{
	int fd = install_driver_fd(true);

	if (fd < 0)
		return 1;
	if (ioctl(fd, KSU_IOCTL_XECHIDE_STATS, &stats) < 0) {
		fprintf(stderr, "xechide-ctl: STATS failed: %s\n", strerror(errno));
		close(fd);
		return 5;
	}
	close(fd);
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
		"  apply     read one struct xechide_config (%zu bytes) from stdin: write the\n"
		"            module config file, then apply to the kernel if its driver exists\n"
		"  set       read one config from stdin and apply it to the kernel only\n"
		"  get       write the live kernel configuration to stdout\n"
		"  file-get  write the module configuration file to stdout\n"
		"  status    print module/kernel presence and the config size (key=value)\n"
		"  stats     print the kernel counters\n"
		"  version   print this helper's ABI version\n"
		"must run as root\n",
		argv0, sizeof(struct xechide_config));
}

int main(int argc, char **argv)
{
	const char *cmd;

	if (argc != 2) {
		usage(stderr, argv[0]);
		return 2;
	}
	cmd = argv[1];

	if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) {
		usage(stdout, argv[0]);
		return 0;
	}
	if (!strcmp(cmd, "version")) {
		printf("xechide-ctl abi %u, config %zu bytes, max %u rules\n", XECHIDE_ABI_VERSION,
		       sizeof(struct xechide_config), XECHIDE_MAX_RULES);
		return 0;
	}

	if (!strcmp(cmd, "apply"))
		return cmd_apply();
	if (!strcmp(cmd, "set"))
		return cmd_set();
	if (!strcmp(cmd, "get"))
		return cmd_get();
	if (!strcmp(cmd, "file-get"))
		return cmd_file_get();
	if (!strcmp(cmd, "status"))
		return cmd_status();
	if (!strcmp(cmd, "stats"))
		return cmd_stats();

	usage(stderr, argv[0]);
	return 2;
}
