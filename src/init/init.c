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
#include <ctype.h>
#include <glob.h>

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

//Put right system_* info here can save aroung ~60ms bootkpi
static const char *ufs_patterns[] = {"/dev/sd*42", "/dev/sd*22", "/dev/sd*6", "/dev/sd*4", "/dev/sd*"};

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))

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
	char slot_suffix[2];
	int mode;
};

static struct cmd_params cmd;

struct MountPoint {
	const char *name;
	const char *where;
	const char *type;
	unsigned long flags;
	const char *options;
};

struct MountPoint mount_table[] = {
#ifdef FIRMWARE_MOUNT
	{"PARTLABEL=modem_a", "/firmware", "vfat", 0, ""},
	{"PARTLABEL=modem_b", "/firmware", "vfat", 0, ""},
	{"PARTLABEL=modem_a", "/firmware/qcom/sa8775p", "vfat", 0, ""},
	{"PARTLABEL=modem_b", "/firmware/qcom/sa8775p", "vfat", 0, ""},
#endif

#ifdef VENDOR_DSP_MOUNT
	{"PARTLABEL=dsp_a", "/vendor/dsp", "ext4", 0, "context=system_u:object_r:dsp_file_t:s0"},
	{"PARTLABEL=dsp_b", "/vendor/dsp", "ext4", 0, "context=system_u:object_r:dsp_file_t:s0"},
#endif
};

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

/**
 * skip_spaces - Removes leading whitespace from @str.
 * @str: The string to be stripped.
 *
 * Returns a pointer to the first non-whitespace character in @str.
 */
char *skip_spaces(const char *str)
{
	while (isspace(*str))
		++str;
	return (char *)str;
}
/*
 * Parse a string to get a param value pair.
 * You can use " around spaces, but can't escape ".
 * Hyphens and underscores equivalent in parameter names.
 */
char *next_arg(char *args, char **param, char **val)
{
	unsigned int i, equals = 0;
	int in_quote = 0, quoted = 0;

	if (*args == '"') {
		args++;
		in_quote = 1;
		quoted = 1;
	}

	for (i = 0; args[i]; i++) {
		if (isspace(args[i]) && !in_quote)
			break;
		if (equals == 0) {
			if (args[i] == '=')
				equals = i;
		}
		if (args[i] == '"')
			in_quote = !in_quote;
	}

	*param = args;
	if (!equals)
		*val = NULL;
	else {
		args[equals] = '\0';
		*val = args + equals + 1;

		/* Don't include quotes in value. */
		if (**val == '"') {
			(*val)++;
			if (args[i-1] == '"')
				args[i-1] = '\0';
		}
	}
	if (quoted && args[i-1] == '"')
		args[i-1] = '\0';

	if (args[i]) {
		args[i] = '\0';
		args += i + 1;
	} else
		args += i;

	/* Chew up trailing spaces. */
	return skip_spaces(args);
}

static bool find_the_device(char* devname, char* token)
{
	bool ret = false;
	blkid_probe probe = blkid_new_probe();
	if (!probe)
	{
		log_kmsg("blkid_new_probe failed");
		goto out;
	}
	blkid_probe_enable_superblocks(probe, 0);
	blkid_probe_enable_partitions(probe, 1);
	blkid_probe_set_partitions_flags(probe, BLKID_PARTS_ENTRY_DETAILS);
	int fd = open(devname, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
	{
		log_kmsg("open device %s failed, errno %d\n", devname, errno);
		goto out;
	}
	blkid_probe_set_device(probe, fd, 0, 0);
	int rc = blkid_do_safeprobe(probe);
	if (rc)
	{
		log_kmsg("blkid_do_safeprobe failed");
		goto out;
	}
	char* val;
	if (!strncmp(token, "PARTUUID", strlen("PARTUUID")) &&
			!blkid_probe_lookup_value (probe, "PART_ENTRY_UUID", &val, NULL) &&
			!strncmp (val, (token + sizeof ("PARTUUID")), strlen(val)))
		ret = true;
	else if (!strncmp(token, "PARTLABEL", strlen("PARTLABEL")) &&
			!blkid_probe_lookup_value (probe, "PART_ENTRY_NAME", &val, NULL) &&
			!strncmp (val, (token + sizeof ("PARTLABEL")), strlen(val)))
		ret = true;
out:
	if (probe)
		blkid_free_probe(probe);
	safe_close(fd);
	return ret;
}

static char* get_device_name(char* token)
{
	char* dev = NULL;
	if (!strncmp(token, "PARTUUID", strlen("PARTUUID")) ||
		!strncmp(token, "PARTLABEL", strlen("PARTLABEL"))) {
		glob_t block_device_list;

		for (size_t  i = 0; i < (sizeof(ufs_patterns)/sizeof(ufs_patterns[0])); ++i)
			glob(ufs_patterns[i],i ? GLOB_APPEND : 0 , NULL, &block_device_list);
		for (size_t i = 0; i < block_device_list.gl_pathc; ++i) {
			if (find_the_device(block_device_list.gl_pathv[i], token)) {
				dev = strdup(block_device_list.gl_pathv[i]);
				break;
			}
		}
		globfree(&block_device_list);
	}
	else {
		//Slow path
		dev = strdup(blkid_get_devname(NULL, token, NULL));
	}
	return dev;
}

/**
 * dm_replace_partuuid_by_dev_num - Replace PARTUUID by Blkid.
 * @cmd_partuuid: dm-table params from cmdline, it include PARTUUID.
 * @cmd_new: a new dm-table params with Blkid.
 *
 */
static void dm_replace_partuuid_by_dev_num(char *cmd_partuuid, char cmd_new[])
{
	char *cmd_temp = cmd_partuuid;
	int i=0,j=0;
	char temp_all[CMD_MAX];
	char temp_partuuid[CMD_MAX];
	char *dev_num = NULL;

	for(i=0; i<3; i++)
	{
		cmd_temp = strchr(cmd_temp, ' ');

		if(i == 1){
			for(j=0; ; j++){
				temp_partuuid[j] = cmd_temp[j+1];
				if(temp_partuuid[j] == ' '){
					break;
				}
			}
			temp_partuuid[j]='\0';
		}
		cmd_temp++;
	}

	dev_num = get_device_name(temp_partuuid);
	// get a new verity table
	snprintf(temp_all, CMD_MAX, "%c %s %s %s", cmd_partuuid[0], dev_num, dev_num, cmd_temp);
	strlcpy(cmd_new, temp_all, strlen(temp_all)+1);

	return;
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
	const char dm_str[] = "dm-mod.create";
	const char root_str[] = "root";
	const char uuid_str[] = "PARTUUID=";
	const char lable_str[] = "LABEL=";
	const char partlable_str[] = "PARTLABEL=";
	const char init_str[] = "init";
	const char fstype_str[] = "rootfstype";
	const char mode_str[] = "early-ramdisk.mode";
	const char slot_str[] = "androidboot.slot_suffix";
	char mode[CMD_MAX] = {0};
	char *buf = NULL;

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

	cmdline[LINE_MAX - 1] = '\0';
	buf = skip_spaces(cmdline);
	while (*buf) {
		char *param, *val;
		buf = next_arg(buf, &param, &val);
		if (!val && !strcmp(param, "--"))
			break;

		if (!strcmp(param, dm_str) && val) {
			ret = strlcpy(cmd->dm.buf, val, strlen(val) + 1);
			if(!dm_cmd_parse(&cmd->dm))
				cmd->dm.enable = true;
		}
		if (!strcmp(param, root_str) && val) {
			ret = strlcpy(cmd->rootfs.root, val, strlen(val) + 1);
			if(!strncmp(cmd->rootfs.root, uuid_str, strlen(uuid_str)) || !strncmp(cmd->rootfs.root, lable_str, strlen(lable_str))) {
				cmd->rootfs.root_alias = 1;
			}
		}
		if (!strcmp(param, init_str) && val) {
			ret = strlcpy(cmd->rootfs.init, val, strlen(val) + 1);
		}
		if (!strcmp(param, fstype_str) && val)
			ret = strlcpy(cmd->rootfs.fstype, val, strlen(val) + 1);
		if (!strcmp(param, mode_str) && val) {
			cmd->mode = atoi(val);
			if(cmd->mode <= 0) {
				log_info("Use single thread load modules\n");
				cmd->mode = 0;
			}
		}
		if (!strcmp(param, slot_str) && val)
			ret = strlcpy(cmd->slot_suffix, val, strlen(val) + 1);
		if (!strcmp(param, "ro")) {
			cmd->rootfs.flag |= MS_RDONLY;
		}
		if (!strcmp(param, "rw")) {
			cmd->rootfs.flag &= ~MS_RDONLY;
		}
	}

	if(!strncmp(cmd->rootfs.root, uuid_str, strlen(uuid_str)) ||
			!strncmp(cmd->rootfs.root, lable_str, strlen(lable_str)) ||\
			!strncmp(cmd->rootfs.root, partlable_str, strlen(partlable_str))) {
		cmd->rootfs.root_alias = 1;
	}

	if (0 == strlen(cmd->rootfs.init)) {
		ret = strlcpy(cmd->rootfs.init, DEFAULT_INIT, strlen(DEFAULT_INIT) + 1);
		log_kmsg("use default init: %s\n", cmd->rootfs.init);
	}
	if (0 == strlen(cmd->rootfs.fstype)) {
		ret = strlcpy(cmd->rootfs.fstype, DEFAULT_FSTYPE, strlen(DEFAULT_FSTYPE) + 1);
		log_kmsg("use default fstype: %s\n", cmd->rootfs.fstype);
	}
	if (0 == strlen(cmd->rootfs.root)) {
		log_kmsg("get root device failed!\n");
		ret = -1;
	}

out:
	safe_close(fd);
	return ret;
}

static int rootfs_alias_setup(struct rootfs_params *rootfs)
{
	char *root_dev = NULL;

	root_dev = get_device_name(rootfs->root);
	if(!root_dev) {
		log_kmsg("Can't find rootfs device: %s\n", rootfs->root);
		return -EINVAL;
	}

	strlcpy(rootfs->root, root_dev, strlen(root_dev) + 1);
	free(root_dev);
	rootfs->root_alias = 0;
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

static void early_mount(void) {
	log_kmsg("early_mount called\n");
	pid_t pid;
	pid = fork();
	if(pid < 0)
		log_kmsg("fork mount process failed\n");
	else if(pid == 0) {
		char* device = NULL;
		char* device_name = "/dev/sde12";
		int fd = 0;

		for (int i = 0; i < 100; i++) {
			fd = access(device_name, F_OK);
			if (fd < 0) {
				log_kmsg("access path %s failed, errno %d\n", device_name, errno);
				usleep(5000);
			}
			else
				break;
		}

		int mount_start = 0;
		int retry_time = 0;
		!strncmp(cmd.slot_suffix, "_a", strlen("_a")) ? (mount_start=0) : (mount_start=1);

		for (int j = mount_start; j < ARRAY_SIZE(mount_table); j = j + 2) {
			device = get_device_name(mount_table[j].name);
			log_kmsg("Found the name %s, by device %s\n", mount_table[j].name, device);

#ifdef FIRMWARE_MOUNT
			if (!strncmp(mount_table[j].where, "/firmware/qcom/sa8775p", strlen("/firmware/qcom/sa8775p"))) {
				int ret=0;
				if (access("/firmware/qcom", F_OK) < 0)
					ret = mkdir("/firmware/qcom/", 0755) || mkdir("/firmware/qcom/sa8775p", 0755);
				else if (access("/firmware/qcom/sa8775p", F_OK) < 0)
					ret = mkdir("/firmware/qcom/sa8775p", 0755);

				if (ret != 0)
					log_kmsg("Failed to prepare /firmware/qcom/sa8775p folder, errno is %d\n", errno);
			}
#endif

retry:
			if (mount(device, mount_table[j].where, mount_table[j].type, mount_table[j].flags, mount_table[j].options)) {
				log_kmsg("mount %s failed errno %d\n", mount_table[j].where, errno);
#ifdef VENDOR_DSP_MOUNT
				if ((errno == 22) && (retry_time < 100)) {
					retry_time++;
					usleep(5000);
					goto retry;
				}
#endif
			}
			else
				log_kmsg("mount %s success\n", mount_table[j].where);
		}
		exit(0);
	}
	return;
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
	memset(&(cmd.rootfs.root), 0, sizeof(cmd.rootfs.root));
	memset(&(cmd.rootfs.init), 0, sizeof(cmd.rootfs.init));
	memset(&(cmd.rootfs.fstype), 0, sizeof(cmd.rootfs.fstype));
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
	// for erofs && avb disabled case, use overlayfs to make /root writable
	if (!cmd.dm.enable && !strcmp(cmd.rootfs.fstype, "erofs"))
	{
		char *data_dev = NULL;
		data_dev = blkid_get_devname(NULL, "PARTLABEL=userdata", NULL);
		if(!data_dev) {
			log_kmsg("Can't find userdata as writable backend\n");
		}
		ret = mount(data_dev, "/realroot/data", "ext4", MS_NODEV | MS_NOATIME, "discard");
		if (ret < 0) {
			log_kmsg("mount userdata %s failed errno is %d\n", data_dev, errno);
		}
		ret = mkdir("/realroot/data/overlay/", 0755) ||
			mkdir("/realroot/data/overlay/upper", 0755) ||
			mkdir("/realroot/data/overlay/workdir", 0755);
		if (ret != 0) {
			log_kmsg("Failed to prepare overlay folder, errno is %d\n", errno);
		}
		ret = mount("overlay", "/realroot", "overlay", MS_NOATIME, "lowerdir=/realroot,upperdir=/realroot/data/overlay/upper/,workdir=/realroot/data/overlay/workdir");
		if (ret < 0) {
			log_kmsg("mount overlayfs %s failed errno is %d\n", data_dev, errno);
		}
		else {
			log_kmsg("mount overlayfs %s done\n", cmd.rootfs.fstype);
		}
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

#ifdef VFIO_BIND_DEVICE
	pid = fork();
	if(pid < 0)
		log_kmsg("fork process for vfio bind device fail\n");
	else if(pid == 0) {
		char* vfio_name = "/sys/module/vfio";
		int fd = 0;
		for (int i = 0; i < 100; ++i) {
			fd = access(vfio_name, F_OK);
			if (fd < 0) {
				log_kmsg("access path %s failed, errno %d\n", vfio_name, errno);
				usleep(5000);
			}
			else
				break;
		}
		log_kmsg("run vfio-device-bind.sh start\n");
		if (execl("/bin/sh", "sh", "/usr/bin/vfio-device-bind.sh", NULL) < 0)
			log_kmsg("run vfio-device-bind.sh fail\n");
		exit(0);
	}
#endif

	log_close();
	mount_unsetup();

#if defined(VENDOR_DSP_MOUNT) || defined(FIRMWARE_MOUNT)
	early_mount();
#endif

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
	char dm_params_blkid[CMD_MAX];

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
		log_kmsg("Update verity table from PARTUUID to blkid\n");
		dm_replace_partuuid_by_dev_num(cmd.dm.target_args_array[i],dm_params_blkid);
		cmd.dm.target_args_array[i] = dm_params_blkid;
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
	int ret = 0;

	for(; count < COND_CHECK_MAX; count++) {
		if(cmd.rootfs.root_alias) {
			ret = rootfs_alias_setup(&cmd.rootfs);
			if(ret < 0) {
				usleep(200);
				continue;
			}
		}
		if(!access(cmd.rootfs.root, F_OK))
			break;
		usleep(200);
	}

	if(count)
		log_info("Wait for rootfs device %.1fms\n", (count * 2) / 10.0);
	return 0;
}

TASKLET_DEFINE_CALL("wait_rootfs_tasklet", rootfs_wait_func);
