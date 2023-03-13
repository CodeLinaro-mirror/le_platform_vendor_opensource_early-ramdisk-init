/*
* Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <pwd.h>
#include <stdint.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sched.h>
#include <errno.h>

#include "utils.h"
#include "init.h"

#define KPI_VALUE_PATH          "/sys/kernel/boot_kpi/kpi_values"
#define LOG_PATH				LOG_DIR"/early_ramdisk_init.log"

#define MSG_LEN					128
#define DEFAULT_INIT			"/sbin/init"
#define DEFAULT_FSTYPE			"ext4"
#define LINE_MAX				2048
#define CMD_MAX					64
#define FS_FLAG_MAX				2
#define FS_RD					0
#define FS_RW					1

struct rootfs_params {
	char root[CMD_MAX];
	char fstype[CMD_MAX];
	char init[CMD_MAX];
	int flag;
};

struct cmd_params {
	struct rootfs_params rootfs;
	int mode;
};

static struct cmd_params cmd;

static void inline write_marker(const char* name)
{
	int fd = -1;

	fd = open(KPI_VALUE_PATH, O_WRONLY);
	if (fd > 0) {
		if(write(fd, name, strlen(name)) < 0)
			perror("write boot marker failed:");
	} else {
		log_warn("open boot marker for name %s failed: %s\r\n",
				name, strerror(errno));
	}
	safe_close(fd);

	return;
}

static void get_mount_flag(char *cmdline, int *flag)
{
	int ret, i;
	char *ptr;
	const char flag_name[FS_FLAG_MAX][16] = {{" ro "}, {" rw "}};

	if(strstr(cmdline, flag_name[FS_RD]))
		*flag |= MS_RDONLY;
	else if(strstr(cmdline, flag_name[FS_RW]))
		*flag &= ~MS_RDONLY;
}

static int get_cmd_value(char *cmdline, const char *name, char *var)
{
	int ret, cmd_len, val_len;
	char *ptr;

	ptr = strstr(cmdline, name);
	if(!ptr) {
		log_info("cannot find %s\n", name);
		return -EINVAL;
	}

	cmd_len = val_len = strlen(name);
	while(*(ptr + val_len) != ' ')
		val_len++;
	val_len -= cmd_len;
	if(val_len > CMD_MAX) {
		log_kmsg("%s value execeed: %d!\n", name, CMD_MAX);
		 return -EINVAL;
	}

	return strlcpy(var, ptr + cmd_len, val_len + 1);
}

static int rootfs_cmd_setup(struct cmd_params *cmd)
{
	int fd;
	int ret, cmd_len, val_len;
	char cmdline[LINE_MAX];
	char *pt;
	const char root_str[] = " root=";
	const char init_str[] = " init=";
	const char fstype_str[] = " rootfstype=";
	const char mode_str[] = " early-ramdisk.mode=";
	char mode[CMD_MAX] = {0};

	fd = open("/proc/cmdline", O_RDONLY|O_CLOEXEC);
	if(fd < 0) {
		log_kmsg("open cmdline fail: %d\n", errno);
		return errno;
	}

	ret = read(fd, cmdline, LINE_MAX);
	if(ret < 0) {
		log_kmsg("read cmdline fail: %d\n", errno);
		return errno;
	}

	/* get root= from cmdline */
	ret = get_cmd_value(cmdline, root_str, cmd->rootfs.root);
	if(ret < 0) {
		log_kmsg("get root device failed!\n");
		goto out;
	}

	/* get rootfstype= from cmdline */
	ret = get_cmd_value(cmdline, fstype_str, cmd->rootfs.fstype);
	if(ret < 0) {
		ret = strlcpy(cmd->rootfs.fstype, DEFAULT_FSTYPE, strlen(DEFAULT_FSTYPE) + 1);
		log_kmsg("use default fstype: %s\n", cmd->rootfs.fstype);
	}

	get_mount_flag(cmdline, &cmd->rootfs.flag);

	/* get init from cmdline */
	ret = get_cmd_value(cmdline, init_str, cmd->rootfs.init);
	if(ret < 0) {
		ret = strlcpy(cmd->rootfs.init, DEFAULT_INIT, strlen(DEFAULT_INIT) + 1);
		log_kmsg("use default init: %s\n", cmd->rootfs.init);
	}

	/* get mode from cmdline */
	ret = get_cmd_value(cmdline, mode_str, mode);
	if(ret < 0) {
		log_info("Use single thread load modules\n");
		cmd->mode = 0;
		ret = 0;
	} else {
		cmd->mode = atoi(mode);
		if(cmd->mode <= 0) {
			log_info("Use single thread load modules\n");
			cmd->mode = 0;
		}
	}

out:
	safe_close(fd);
	return ret;
}

static int mount_setup(void)
{
	if(mount("tmpfs", LOG_DIR, "tmpfs", MS_NOSUID|MS_NODEV|MS_STRICTATIME,
				"mode=755"))
		perror("mount LOG_DIR failed:");

	if(mount("devtmpfs", "/dev", "devtmpfs", MS_SILENT, NULL)) {
		perror("early-ramdisk-init: mount devtmpfs failed: ");
		return errno;
	}

	if(mount("sysfs", "/sys", "sysfs", MS_SILENT, NULL)) {
		log_kmsg("mount sysfs failed: %d\n", errno);
		return errno;
	}

	if(mount("proc", "/proc", "proc", MS_SILENT, NULL)) {
		log_kmsg("mount proc failed: %d\n", errno);
		return errno;
	}

	return 0;
}

static void mount_unsetup(void)
{
	umount("/proc");
	umount("/sys");
	umount("/dev");
}

int main(int argc, char* argv[])
{
	int ret;
	char root[CMD_MAX] = {0};
	char fstype[CMD_MAX] = {0};
	char init[CMD_MAX] = {0};
	char real_log[CMD_MAX] = {0};
	int flag = 0;
	pid_t pid = -1;

	if(ret = mount_setup())
		return ret;

	ret = log_setup(LOG_PATH);
	if(ret < 0)
		log_kmsg("open log file: %s fail!\n", LOG_PATH);

	write_marker("E - early-ramdisk start up");
	log_kmsg("start\n");

	memset(&cmd, 0, sizeof(struct cmd_params));
	ret = rootfs_cmd_setup(&cmd);
	if(ret < 0)
		return ret;

	fast_modules_load(cmd.mode);
	write_marker("E - early-ramdisk modules done");
	log_kmsg("load modules done\n");

	log_kmsg("root device: %s, fstype: %s, - 0x%X\n",
			cmd.rootfs.root, cmd.rootfs.fstype, cmd.rootfs.flag);
	log_kmsg("Run %s as rootfs init\n", cmd.rootfs.init);

	if(mount(cmd.rootfs.root, "/realroot", cmd.rootfs.fstype,
				cmd.rootfs.flag, NULL)) {
		log_kmsg("mount rootfs failed: %d\n", errno);
		return errno;
	}

	snprintf(real_log, CMD_MAX, "/realroot%s", LOG_DIR);
	if(mount(LOG_DIR, real_log, "bind", MS_BIND | MS_REC, NULL))
		log_kmsg("mount %s logfs failed: %d\n", real_log, errno);

	if(chroot("/realroot")) {
		log_kmsg("chroot rootfs failed: %d\n", errno);
		return errno;
	}

#ifdef EARLY_INIT
	pid = fork();
	if(pid < 0)
		log_kmsg("fork modules load process failed\n");
	else if(pid == 0) {
		execl("/usr/sbin/early_init", "/usr/sbin/early_init", NULL);
		return 0;
	}
#endif

	log_close();
	mount_unsetup();
	if(execl(cmd.rootfs.init, cmd.rootfs.init, NULL)) {
		return errno;
	}

	return 0;
}

int rootfs_wait_func(void *data)
{
	int count = 0;

	for(; count < COND_CHECK_MAX; count++) {
		if(!access(cmd.rootfs.root, F_OK))
			break;
		usleep(200);
	}

	if(count)
		log_info("Wait for rootfs device %.1fms\n", (count * 2) / 10.0);
	return 0;
}

TASKLET_DEFINE_CALL("wait_rootfs_tasklet", rootfs_wait_func);
