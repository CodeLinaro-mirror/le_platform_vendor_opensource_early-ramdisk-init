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
#include <linux/dm-ioctl.h>
#include <blkid/blkid.h>

#include "utils.h"
#include "init.h"

#define KPI_VALUE_PATH          "/sys/kernel/boot_kpi/kpi_values"
#define LOG_PATH				LOG_DIR"/early_ramdisk_init.log"
#define DM_DEVICE				"/dev/mapper/control"

#define MSG_LEN					128
#define NAME_MAX				128
#define DEFAULT_INIT			"/sbin/init"
#define DEFAULT_FSTYPE			"ext4"
#define LINE_MAX				2048
#define CMD_MAX					512
#define FS_FLAG_MAX				2
#define FS_RD					0
#define FS_RW					1
#define DM_BUF_LEN				1024
#define DM_MAX_TARGETS			5

struct rootfs_params {
	char root[CMD_MAX];
	char fstype[CMD_MAX];
	char init[CMD_MAX];
	int root_alias;
	int flag;
};

struct dm_params {
	char buf[CMD_MAX];
	bool enable;
	char *name;
	char *uuid;
	char *minor;
	int  flags;
	struct dm_target_spec sp[DM_MAX_TARGETS];
	char *target_args_array[DM_MAX_TARGETS];
	int target_count;
};

struct cmd_params {
	struct rootfs_params rootfs;
	struct dm_params dm;
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

static int get_cmd_value(char *cmdline, const char *name, char *var,
		const char separator)
{
	int ret, cmd_len, val_len;
	char *ptr, *ptmp;

	ptr = strstr(cmdline, name);
	if(!ptr) {
		log_info("cannot find %s\n", name);
		return -EINVAL;
	}

	cmd_len = strlen(name);
	ptmp = ptr + cmd_len;
	while(*ptmp && (*ptmp != separator))
		ptmp++;

	val_len = ptmp - ptr - cmd_len;
	if(val_len > CMD_MAX) {
		log_kmsg("%s value execeed: %d!\n", name, CMD_MAX);
		 return -EINVAL;
	}

	return strlcpy(var, ptr + cmd_len, val_len + 1);
}

static char *dm_table_parse_entry(struct dm_params *dm, char *entry)
{
	const unsigned int n = dm->target_count - 1;
	struct dm_target_spec *sp = &dm->sp[n];
	unsigned int i;
	char *filed[4];
	char *next;

	next = filed[0] = entry;
	for(i = 1; i < 4; i++) {
		filed[i] = strchr(next, ' ');
		if(!filed[i])
			return NULL;
		*filed[i] = '\0';
		next = ++filed[i];
	}

	next = strchr(next, ',');
	if(next) {
		*next = '\0';
		next++;
	}

	sp->sector_start = atol(filed[0]);
	sp->length = atol(filed[1]);
	strlcpy(sp->target_type, filed[2], sizeof(sp->target_type));
	dm->target_args_array[n] = filed[3];

	return next;
}

static int dm_table_parse(struct dm_params *dm, char *table)
{

	while(table) {
		if(++dm->target_count > DM_MAX_TARGETS) {
			log_kmsg("too many device-mapper table tagets!\n");
			return -EINVAL;
		}

		table = dm_table_parse_entry(dm, table);
	}

	return 0;
}

static int dm_cmd_parse(struct dm_params *dm)
{
	char *filed[5];
	unsigned int i;
	char *next;

	next = filed[0] = dm->buf;
	for(i = 1; i < 5; i++) {
		filed[i] = strchr(next, ',');
		if(!filed[1])
			return -EINVAL;
		*filed[i] = '\0';
		next = ++filed[i];
	}
	dm->name = filed[0];
	dm->uuid = filed[1];
	dm->minor = filed[2];

	if(strlen(dm->minor))
		dm->flags |= DM_PERSISTENT_DEV_FLAG;

	if(!strcmp(filed[3], "ro"))
		dm->flags |= DM_READONLY_FLAG;
	else if (strcmp(filed[3], "rw"))
		return -EINVAL;

	return dm_table_parse(dm, filed[4]);
}

static int rootfs_cmd_setup(struct cmd_params *cmd)
{
	int fd;
	int ret, cmd_len, val_len;
	char cmdline[LINE_MAX] = {0};
	char *pt;
	const char dm_str[] = " dm-mod.create=\"";
	const char root_str[] = " root=";
	const char uuid_str[] = "PARTUUID=";
	const char lable_str[] = "LABEL=";
	const char partlable_str[] = "PARTLABEL=";
	const char init_str[] = " init=";
	const char fstype_str[] = " rootfstype=";
	const char mode_str[] = " early-ramdisk.mode=";
	char mode[CMD_MAX] = {0};

	fd = open("/proc/cmdline", O_RDONLY|O_CLOEXEC);
	if(fd < 0) {
		log_kmsg("open cmdline fail: %d\n", errno);
		return errno;
	}

	ret = read(fd, cmdline, LINE_MAX - 1);
	if(ret < 0) {
		log_kmsg("read cmdline fail: %d\n", errno);
		return errno;
	}

	ret = get_cmd_value(cmdline, dm_str, cmd->dm.buf, '\"');
	if(ret < 0) {
		log_info("dm-verity disabled!\n");
	} else {
		if(!dm_cmd_parse(&cmd->dm))
			cmd->dm.enable = true;
	}

	/* get root= from cmdline */
	ret = get_cmd_value(cmdline, root_str, cmd->rootfs.root, ' ');
	if(ret < 0) {
		log_kmsg("get root device failed!\n");
		goto out;
	}

	if(!strncmp(cmd->rootfs.root, uuid_str, strlen(uuid_str)) ||
			!strncmp(cmd->rootfs.root, lable_str, strlen(lable_str)) ||\
			!strncmp(cmd->rootfs.root, partlable_str, strlen(partlable_str))) {
		cmd->rootfs.root_alias = 1;
	}

	/* get rootfstype= from cmdline */
	ret = get_cmd_value(cmdline, fstype_str, cmd->rootfs.fstype, ' ');
	if(ret < 0) {
		ret = strlcpy(cmd->rootfs.fstype, DEFAULT_FSTYPE, strlen(DEFAULT_FSTYPE) + 1);
		log_kmsg("use default fstype: %s\n", cmd->rootfs.fstype);
	}

	get_mount_flag(cmdline, &cmd->rootfs.flag);

	/* get init from cmdline */
	ret = get_cmd_value(cmdline, init_str, cmd->rootfs.init, ' ');
	if(ret < 0) {
		ret = strlcpy(cmd->rootfs.init, DEFAULT_INIT, strlen(DEFAULT_INIT) + 1);
		log_kmsg("use default init: %s\n", cmd->rootfs.init);
	}

	/* get mode from cmdline */
	ret = get_cmd_value(cmdline, mode_str, mode, ' ');
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

static int rootfs_alias_setup(struct rootfs_params *rootfs)
{
	char *root_dev = NULL;

	root_dev = blkid_get_devname(NULL, rootfs->root, NULL);
	if(!root_dev) {
		log_kmsg("Can't find rootfs device: %s\n", rootfs->root);
		return -EINVAL;
	}

	strlcpy(rootfs->root, root_dev, strlen(root_dev) + 1);
	free(root_dev);
	return 0;
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

	if(cmd.rootfs.root_alias) {
		ret = rootfs_alias_setup(&cmd.rootfs);
		if(ret < 0)
			return ret;
	}

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

	if (chdir("/realroot")) {
		log_kmsg("failed to change directory to new root");
		return -1;
	}

	if (mount("/realroot", "/", NULL, MS_MOVE, NULL) < 0) {
		log_kmsg("failed to mount moving %s to /", "/realroot");
		return -1;
	}

	if (chroot(".")) {
		log_kmsg("failed to change root");
		return -1;
	}

	if (chdir("/")) {
		log_kmsg("cannot change directory to %s", "/");
		return -1;
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

static inline void dm_fill_ioctl(struct dm_ioctl *ctl, int buffer_size)
{
	ctl->data_size = buffer_size;
	ctl->data_start = sizeof(struct dm_ioctl);
	ctl->version[0] = DM_VERSION_MAJOR;
	ctl->version[1] = DM_VERSION_MINOR;
	ctl->version[2] = DM_VERSION_PATCHLEVEL;
	ctl->flags = cmd.dm.flags;
	strlcpy(ctl->name, cmd.dm.name, strlen(cmd.dm.name) + 1);
	strlcpy(ctl->uuid, cmd.dm.uuid, strlen(cmd.dm.uuid) + 1);
}

int dm_create_roots(void *data)
{
	int fd = -1;
	int ret = 0;
	int i = 0;
	char *dm_buffer = NULL;
	struct dm_ioctl *ctl;
	struct dm_target_spec *sp;

	if(!cmd.dm.enable)
		return -EINVAL;

	dm_buffer = malloc(DM_BUF_LEN);
	if(!dm_buffer) {
		log_kmsg("Device Mapper: alloc dm ioctl buffer failed!\n");
		return -ENOMEM;
	}

	fd = open(DM_DEVICE, O_RDWR | O_CLOEXEC);
	if(fd < 0) {
		log_kmsg("Open device mapper: %s failed\n", DM_DEVICE);
		return fd;
	}

	memset(dm_buffer, 0, DM_BUF_LEN);
	ctl = (struct dm_ioctl *)dm_buffer;
	dm_fill_ioctl(ctl, DM_BUF_LEN);

	if((ret = ioctl(fd, DM_DEV_CREATE, ctl))) {
		log_kmsg("Device mapper create %s failed: %d!\n", ctl->name, ret);
		goto create_fail;
	}

	memset(dm_buffer, 0, DM_BUF_LEN);
	dm_fill_ioctl(ctl, DM_BUF_LEN);
	sp = (struct dm_target_spec *)(dm_buffer + sizeof(struct dm_ioctl));
	ctl->target_count = cmd.dm.target_count;
	for(i = 0; i < ctl->target_count; i++) {
		*sp = cmd.dm.sp[i];
		sp++;
		if(((char *)sp + strlen(cmd.dm.target_args_array[i]))
					> (dm_buffer + DM_BUF_LEN)) {
			log_kmsg("Device mapper buffer size too small, Please increase DM_BUF_LEN\n");
			goto load_fail;
		}
		memcpy(sp, cmd.dm.target_args_array[i], strlen(cmd.dm.target_args_array[i]));
		sp = (struct dm_target_spec *)((char *)sp + strlen(cmd.dm.target_args_array[i]));
	}

	for(i = 0; i < COND_CHECK_MAX; i++) {
		if((ret = ioctl(fd, DM_TABLE_LOAD, ctl)) == 0)
			break;
		usleep(200);
	}

	if(ret) {
		log_kmsg("Device mapper TABLE_LOAD failed: %d\n", ret);
			goto load_fail;
	}

	memset(dm_buffer, 0, DM_BUF_LEN);
	dm_fill_ioctl(ctl, DM_BUF_LEN);

	if((ret = ioctl(fd, DM_DEV_SUSPEND, ctl))) {
		log_kmsg("Device mapper Active failed: %d\n", ret);
		goto active_fail;
	}

	free(dm_buffer);
	return ret;

active_fail:
load_fail:
	dm_fill_ioctl(ctl, DM_BUF_LEN);
	ioctl(fd, DM_DEV_REMOVE, ctl);

create_fail:
	safe_close(fd);
	free(dm_buffer);
	return ret;
}
TASKLET_DEFINE_CALL("dm_create_tasklet", dm_create_roots);

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
