/*
* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/mount.h>
#include <blkid/blkid.h>
#include <ctype.h>
#include <glob.h>

#include "utils.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))
#define FIRMWARE_MOUNT_DIR			"/lib/firmware/qcom"

static int early_init_func(void *data)
{
	execl("/usr/sbin/early_init", "/usr/sbin/early_init", NULL);
	return 0;
}
TASKLET_LATE_CALL("early_init_tasklet", early_init_func);

static int vfio_bind_device_func(void *data)
{
	char* vfio_name = "/sys/module/vfio";
	int fd = 0;
	for (int i = 0; i < 100; ++i) {
		fd = access(vfio_name, F_OK);
		if (fd < 0) {
			usleep(5000);
		}
		else
			break;
	}

	if (fd < 0) {
		log_kmsg("access path %s failed, errno %d\n", vfio_name, errno);
		exit(EXIT_FAILURE);
	}

	log_kmsg("run vfio-device-bind.sh start\n");
	if (execl("/bin/sh", "sh", "/usr/bin/vfio-device-bind.sh", NULL) < 0)
		log_kmsg("run vfio-device-bind.sh fail\n");

	return 0;
}
TASKLET_LATE_CALL("vfio_bind_device_tasklet", vfio_bind_device_func);

static int mm_vfio_bind_device_func(void *data)
{
	char* vfio_mm_name = "/sys/module/vfio";
	int fd_mm = 0;
	for (int i = 0; i < 100; ++i) {
		fd_mm = access(vfio_mm_name, F_OK);
		if (fd_mm < 0) {
			log_kmsg("access path %s failed, errno %d\n", vfio_mm_name, errno);
			usleep(5000);
		}
		else
			break;
	}
	log_kmsg("run mm-vfio-device-bind.sh start\n");
	if (execl("/bin/sh", "sh", "/usr/bin/mm-vfio-device-bind.sh", NULL) < 0)
		log_kmsg("run mm-vfio-device-bind.sh fail\n");

	return 0;
}
TASKLET_LATE_CALL("mm_vfio_bind_device_tasklet", mm_vfio_bind_device_func);

static void preload_unit(unsigned char type, char* name)
{
	int ret;
	FILE *f;
	char buff[1024];

	if(type == DT_DIR) {
		struct dirent **conf_list;
		int conf_num;
		int i = 0;

		conf_num = scandir(name, &conf_list, NULL, alphasort);
		if(conf_num < 0) {
			log_error("preload %s scandir failed!\n", name);
			free(conf_list);
			return;
		}

		for(i = 2; i < conf_num; i++){
			char name_sub[150]={'\0'};
			strlcpy(name_sub, name, sizeof(name_sub));
			if (strlen(name_sub) > 148)
                            continue;
			name_sub[strlen(name_sub)] = '/';
			name_sub[strlen(name_sub)+1] = '\0';
			strlcpy(name_sub + strlen(name_sub), conf_list[i]->d_name, sizeof(name_sub));

			preload_unit(conf_list[i]->d_type, name_sub);
		}

	}

	f = fopen(name, "r");
	if(f == NULL) {
		log_error("preload %s open failed!\n", name);
		return;
	}

	ret = fread(buff, 1, sizeof(buff), f);

	fclose(f);
}

static int preload_unit_func(void *data)
{
	struct dirent **conf_list;
	int conf_num;
	int ret = 0;
	int j = 0;

	char *load_path[] = {"/lib/systemd/system/", "/etc/systemd/system/"};

	log_kmsg("created preload process\n");
	for (size_t  n = 0; n < (sizeof(load_path)/sizeof(load_path[0])); ++n) {
		log_kmsg("preload units in %s\n", load_path[n]);
		conf_num = scandir(load_path[n], &conf_list, NULL, alphasort);
		if(conf_num < 0) {
			log_error("%s preload scandir failed!\n", load_path[n]);
			free(conf_list);
			continue;
		}

		ret = chdir(load_path[n]);
		if(ret) {
			log_error("%s preload chdir failed!\n", load_path[n]);
			free(conf_list);
			continue;
		}

		for(j = 2; j < conf_num; j++){
			preload_unit(conf_list[j]->d_type, conf_list[j]->d_name);
		}
	}

	log_kmsg("finish preload files");

	return 0;
}
TASKLET_LATE_CALL("preload_unit_tasklet", preload_unit_func);

struct MountPoint mount_table[] = {
#ifdef FIRMWARE_MOUNT
	// Legacy mount dir is /firmware. In later patches we may remove support
	// of mounting modem to /firmware from early ramdisk
	{"PARTLABEL=modem_a", "/firmware", "vfat", 0, ""},
	{"PARTLABEL=modem_b", "/firmware", "vfat", 0, ""},
	// New mount dir which conforms with Linux kernel's firmware search paths
	{"PARTLABEL=modem_a", FIRMWARE_MOUNT_DIR, "vfat", 0, ""},
	{"PARTLABEL=modem_b", FIRMWARE_MOUNT_DIR, "vfat", 0, ""},
#endif

#ifdef VENDOR_DSP_MOUNT
	{"PARTLABEL=dsp_a", "/vendor/dsp", "ext4", 0, "context=system_u:object_r:dsp_file_t:s0"},
	{"PARTLABEL=dsp_b", "/vendor/dsp", "ext4", 0, "context=system_u:object_r:dsp_file_t:s0"},
#endif
};

static int early_mount_func(void *data)
{
	int mount_start = 0;
	int retry_time = 0;
	char *slot_suffix;
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

	slot_suffix = get_slot_suffix();
    mount_start = !strncmp(slot_suffix, "_a", strlen("_a")) ? (0) : (1);

	for (int j = mount_start; j < ARRAY_SIZE(mount_table); j = j + 2) {
		device = get_device_name(mount_table[j].name);
		log_kmsg("Found the name %s, by device %s\n", mount_table[j].name, device);

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

	return 0;
}
TASKLET_LATE_CALL("early_mount_tasklet", early_mount_func);

// socinfo information
static char machine_name[128] = {0};
static int soc_id = -1;

int fetch_socinfo(void *data)
{
	FILE *socinfo_fp = NULL;

	socinfo_fp = fopen("/sys/devices/soc0/soc_id", "r");
	if(socinfo_fp == NULL) {
		log_kmsg("error: can't open /sys/devices/soc0/soc_id\n");
	} else {
		char buf[128];
		if(fgets(buf, sizeof(buf), socinfo_fp) != NULL) {
			soc_id = atoi(buf);
			log_kmsg("socid is %d\n", soc_id);
		} else {
			log_kmsg("error: fgets() return NULL\n");
		}
		fclose(socinfo_fp);
	}
	socinfo_fp = fopen("/sys/devices/soc0/machine", "r");
	if(socinfo_fp == NULL) {
		log_kmsg("error: can't open /sys/devices/soc0/machine\n");
	} else {
		if(fgets(machine_name, sizeof(machine_name), socinfo_fp) != NULL) {
			//Remove trailing newline if present
			size_t len = strlen(machine_name);
			if (len > 0 && machine_name[len-1] == '\n')
				machine_name[len-1] = '\0';
		} else {
			log_kmsg("error: fgets() fetch machine return NULL\n");
		}
		fclose(socinfo_fp);
		log_kmsg("machine is %s\n", machine_name);
	}

	return 0;
}
TASKLET_EARLY_CALL("fetch_socinfo_tasklet", fetch_socinfo);

static int uni_overlayfs(char* path, char* machine, const char* chip)
{
	int ret = 0;
	char lower_dir[128];

	if (machine != NULL && machine[0] == '\0')
		snprintf(lower_dir, sizeof(lower_dir), "lowerdir=/uni/%s%s:%s", chip, path, path);
	else if (chip != NULL && chip[0] == '\0') {
		if (!strcmp(machine_name, "SA_QX_VM"))
			snprintf(lower_dir, sizeof(lower_dir), "lowerdir=/uni/hqx%s:%s", path, path);
		else if (!strcmp(machine_name, "SA_GUNYAH_VM"))
			snprintf(lower_dir, sizeof(lower_dir), "lowerdir=/uni/hgy%s:%s", path, path);
	} else {
		if (!strcmp(machine_name, "SA_QX_VM"))
			snprintf(lower_dir, sizeof(lower_dir), "lowerdir=/uni/hqx/%s%s:%s", chip, path, path);
		else if (!strcmp(machine_name, "SA_GUNYAH_VM"))
			snprintf(lower_dir, sizeof(lower_dir), "lowerdir=/uni/hgy/%s%s:%s", chip, path, path);
	}

	ret = mount("overlay", path, "overlay", MS_NOATIME, lower_dir);
	if(0 != ret) {
		log_kmsg("mount %s error: %d with lowerdir: %s\n", path, errno, lower_dir);
	} else {
		log_kmsg("mount %s overlayfs %s done\n", lower_dir, path);
	}

	return ret;
}

static int uni_bindfs(char* path, char* machine, char* chip)
{
	int ret = 0;
	char lower_dir[128];

	if (machine != NULL && machine[0] == '\0')
		snprintf(lower_dir, sizeof(lower_dir), "lowerdir=/uni/%s%s", chip, path);
	else if (chip != NULL && chip[0] == '\0') {
		if (!strcmp(machine_name, "SA_QX_VM"))
			snprintf(lower_dir, sizeof(lower_dir), "/uni/hqx%s", path);
		else if (!strcmp(machine_name, "SA_GUNYAH_VM"))
			snprintf(lower_dir, sizeof(lower_dir), "/uni/hgy%s", path);
	} else {
		if (!strcmp(machine_name, "SA_QX_VM"))
			snprintf(lower_dir, sizeof(lower_dir), "/uni/hqx/%s%s", chip, path);
		else if (!strcmp(machine_name, "SA_GUNYAH_VM"))
			snprintf(lower_dir, sizeof(lower_dir), "/uni/hgy/%s%s", chip, path);
	}

	ret = mount(lower_dir, path, NULL, MS_BIND, NULL);
	if(0 != ret) {
		log_kmsg("bind %s error: %d with lowerdir: %s\n", path, errno, lower_dir);
	} else {
		log_kmsg("bind %s overlayfs %s done\n", lower_dir, path);
	}

	return ret;
}

static int video_lib_unification_func(void *data)
{
	int ret;
	char lower_dir[128];
	const char *socid_name;
	switch (soc_id) {
		case 532:
		case 533:
		case 534:
		case 535:
			socid_name = "lemans";
			break;
		case 606:
		case 695:
			socid_name = "monaco";
			break;
			default:
			socid_name = NULL;
			break;
	}

	// Handle overlay
	if (!strcmp(machine_name, "SA_QX_VM") || !strcmp(machine_name, "SA_GUNYAH_VM")) {
		//video overlayfs
		uni_overlayfs("/usr/lib", "", socid_name);
		uni_overlayfs("/etc", "", socid_name);

		// security overlay
		uni_overlayfs("/usr/bin", machine_name, "");
		uni_overlayfs("/usr/lib", machine_name, "");

		// common overlay
		//uni_overlayfs("/usr/bin", machine_name, socid_name);
		//uni_overlayfs("/usr/lib", machine_name, socid_name);
		//uni_overlayfs("/etc", machine_name, socid_name);

		// security driver load
		uni_bindfs("/etc/modules-load.d/security_load.conf", machine_name, "");
		uni_bindfs("/etc/fstab", machine_name, "");
	}

	return 0;
}
TASKLET_LATE_CALL("video_lib_unification_tasklet", video_lib_unification_func);
