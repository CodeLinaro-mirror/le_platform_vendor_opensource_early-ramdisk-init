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
#include <limits.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/mount.h>
#include <blkid/blkid.h>
#include <ctype.h>
#include <glob.h>
#include <libkmod.h>

#include "utils.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))
#define FIRMWARE_MOUNT_DIR			"/lib/firmware/qcom"

#define SCSI_LUN_INVALID	(-1)
#define SCSI_SCAN_LUN_MAX	255
#define SCSI_PARAM_MAX		512
#define SCSI_HOST_MAX		32

static int early_init_func(void *data)
{
	execl("/usr/sbin/early_init", "/usr/sbin/early_init", NULL);
	return 0;
}
TASKLET_LATE_CALL("early_init_tasklet", early_init_func);

static void wait_for_systemd_exec(void)
{
	char comm[32];
	FILE *f;
	int i;

	for (i = 0; i < 500; i++) {        /* max 5s */
		f = fopen("/proc/1/comm", "r");
		if (f) {
			memset(comm, 0, sizeof(comm));
			if (fgets(comm, sizeof(comm), f) &&
				strncmp(comm, "systemd", 7) == 0) {
				fclose(f);
				return;
			}
			fclose(f);
		}
		usleep(5000);                   /* 5ms */
	}
	log_kmsg("geni module load: timeout waiting for systemd exec\n");
}

static int geni_modules_load_func(void *data)
{
	/* Load immediately: fast probes with no IOMMU contention */
	static const char *pre_exec[] = {
		"qcom-geni-se",
		"qcom_ccu_qup",
	};
	/* Load after systemd exec: gpi IOMMU probe must not race with execl() */
	static const char *post_exec[] = {
		"gpi",
		"qcom_geni_serial",
		"spi_geni_qcom",
		"i2c-qcom-geni",
	};
	struct kmod_ctx *ctx;
	struct kmod_module *mod;
	unsigned int i;
	int ret;

	ctx = kmod_new(NULL, NULL);
	if(!ctx) {
		log_kmsg("geni module load: kmod_new failed\n");
		return 0;
	}
	kmod_load_resources(ctx);

	for(i = 0; i < ARRAY_SIZE(pre_exec); i++) {
		ret = kmod_module_new_from_name(ctx, pre_exec[i], &mod);
		if(ret) {
			log_kmsg("geni module load: lookup %s failed: %d\n", pre_exec[i], ret);
			continue;
		}

		ret = kmod_module_probe_insert_module(mod, KMOD_PROBE_APPLY_BLACKLIST,
				NULL, NULL, NULL, NULL);
		if(ret < 0)
			log_kmsg("geni module load: probe insert %s failed: %d\n", pre_exec[i], ret);
		else
			log_kmsg("geni module load: %s loaded\n", pre_exec[i]);

		kmod_module_unref(mod);
	}

	wait_for_systemd_exec();

	for(i = 0; i < ARRAY_SIZE(post_exec); i++) {
		ret = kmod_module_new_from_name(ctx, post_exec[i], &mod);
		if(ret) {
			log_kmsg("geni module load: lookup %s failed: %d\n", post_exec[i], ret);
			continue;
		}

		ret = kmod_module_probe_insert_module(mod, KMOD_PROBE_APPLY_BLACKLIST,
				NULL, NULL, NULL, NULL);
		if(ret < 0)
			log_kmsg("geni module load: probe insert %s failed: %d\n", post_exec[i], ret);
		else
			log_kmsg("geni module load: %s loaded\n", post_exec[i]);

		kmod_module_unref(mod);
	}

	kmod_unref(ctx);

	return 0;
}
TASKLET_LATE_CALL("geni_modules_load_tasklet", geni_modules_load_func);

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
	{"PARTLABEL=dsp_a", "/vendor/dsp", "ext4", MS_RDONLY, "context=system_u:object_r:dsp_file_t:s0"},
	{"PARTLABEL=dsp_b", "/vendor/dsp", "ext4", MS_RDONLY, "context=system_u:object_r:dsp_file_t:s0"},
#endif
};

/*
 * wait_disk_io_ready - block until the parent SCSI disk of @device reports a
 * non-zero size, i.e. it is attached for I/O (logged as "[sdX] Attached SCSI
 * disk"). The partition node can exist in sysfs/devtmpfs before the disk is
 * I/O-ready, which makes the first mount fail with EINVAL; waiting on the size
 * avoids blind sleeps. sysfs only, no UFS reads.
 */
static void wait_disk_io_ready(const char *device)
{
	char disk_size_path[64];
	char devname[32] = {0};
	const char *dev_basename = device;
	int w;

	/* extract disk name: /dev/sde28 -> sde */
	if (strncmp(device, "/dev/", 5) == 0)
		dev_basename = device + 5;
	/* strip partition digits at end */
	for (int k = 0; dev_basename[k] && k < (int)sizeof(devname) - 1; k++) {
		if (dev_basename[k] < '0' || dev_basename[k] > '9')
			devname[k] = dev_basename[k];
		else
			break;
	}
	snprintf(disk_size_path, sizeof(disk_size_path),
		 "/sys/class/block/%s/size", devname);

	for (w = 0; w < 2000; w++) {
		FILE *f = fopen(disk_size_path, "r");
		if (f) {
			unsigned long sz = 0;
			if (fscanf(f, "%lu", &sz) == 1 && sz > 0) {
				fclose(f);
				break;
			}
			fclose(f);
		}
		usleep(200);
	}
	if (w > 0)
		log_kmsg("early_mount: %s ready after %.1fms\n",
			 devname, (w * 200) / 1000.0f);
}

/*
 * mount_one - mount a single mount_table entry using the already-resolved
 * @device node. On EINVAL (errno 22) it retries up to 100 times when
 * VENDOR_DSP_MOUNT is set; if @wait_disk_ready, the first retry waits for the
 * parent disk to become I/O-ready instead of sleeping blindly. retry_time is
 * local so it resets per entry.
 */
static void mount_one(const struct MountPoint *mp, const char *device,
		      bool wait_disk_ready)
{
#ifdef VENDOR_DSP_MOUNT
	int retry_time = 0;
#else
	/* retry_time/wait_disk_ready are only used in the DSP retry path */
	(void)wait_disk_ready;
#endif

	while (mount(device, mp->where, mp->type, mp->flags, mp->options)) {
		log_kmsg("mount %s failed errno %d\n", mp->where, errno);
#ifdef VENDOR_DSP_MOUNT
		if ((errno == 22) && (retry_time < 100)) {
			if (wait_disk_ready && retry_time == 0)
				wait_disk_io_ready(device);
			retry_time++;
			usleep(5000);
			continue;
		}
#endif
		return;
	}

	log_kmsg("mount %s success\n", mp->where);
}

static int early_mount_func(void *data)
{
	int mount_start = 0;
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

		mount_one(&mount_table[j], device, false);
		free(device);
		device = NULL;
	}

	return 0;
}
TASKLET_LATE_CALL("early_mount_tasklet", early_mount_func);

/*
 * early_mount_fast_func - SA8797/Nord optimized firmware/DSP mount path.
 *
 * Keep this separate from early_mount_func(), because early_mount_tasklet is a
 * common tasklet used by other platform configs. SA8797 opts into this tasklet
 * explicitly from conf/sa8797/tasklet.late.
 */
static int early_mount_fast_func(void *data)
{
	int mount_start = 0;
	char *slot_suffix;
	char *device = NULL;

	log_kmsg("early_mount: tasklet started\n");

	slot_suffix = get_slot_suffix();
	mount_start = !strncmp(slot_suffix, "_a", strlen("_a")) ? (0) : (1);

	/*
	 * Wait for the first partition we need to mount, using sysfs ONLY.
	 *
	 * IMPORTANT: do NOT call get_device_name() (blkid) here while waiting.
	 * If the partition isn't enumerated yet, blkid's get_device_name() falls
	 * through to blkid_get_devname(NULL,...) -- a full scan of all 234+ block
	 * device nodes, reading ~17KB from each disk. With sde having 82 partitions
	 * this costs ~400ms per call. Calling it in a polling loop is catastrophic.
	 *
	 * Instead: poll sysfs (PARTNAME in uevent, no UFS reads, ~0.1ms per pass)
	 * until the partition appears, THEN resolve to a /dev node once at the end.
	 */
	if (mount_start < ARRAY_SIZE(mount_table)) {
		const char *wait_label = mount_table[mount_start].name;
		int i;

		for (i = 0; i < 2000; i++) {		/* 2000 * 5ms = 10s max */
			device = resolve_partition_sysfs(wait_label);
			if (device) {
				free(device);
				device = NULL;
				break;
			}
			usleep(5000);
		}
		if (i == 2000)
			log_kmsg("early_mount: timeout waiting for %s\n",
				 wait_label);
		else if (i > 0)
			log_kmsg("early_mount: %s appeared after %dms\n",
				 wait_label, i * 5);
	}

	for (int j = mount_start; j < ARRAY_SIZE(mount_table); j = j + 2) {
		/*
		 * sysfs fast-path: PARTNAME lookup, no UFS reads.
		 * Only fall back to blkid if sysfs misses (should not happen
		 * after the wait above confirmed the first partition exists,
		 * since modem and dsp are on the same LUN).
		 */
		device = resolve_partition_sysfs(mount_table[j].name);
		if (!device) {
			log_kmsg("early_mount: sysfs miss for %s, trying blkid\n",
				 mount_table[j].name);
			device = get_device_name(mount_table[j].name);
			log_kmsg("early_mount: blkid resolved %s -> %s\n",
				 mount_table[j].name, device ? device : "(null)");
		} else {
			log_kmsg("early_mount: sysfs resolved %s -> %s\n",
				 mount_table[j].name, device);
		}

		log_kmsg("Found the name %s, by device %s\n",
			 mount_table[j].name, device ? device : "(null)");

		if (!device) {
			log_kmsg("early_mount: no device for %s, skipping\n",
				 mount_table[j].name);
			continue;
		}

		mount_one(&mount_table[j], device, true);
		free(device);
		device = NULL;
	}

	return 0;
}
TASKLET_LATE_CALL("early_mount_fast_tasklet", early_mount_fast_func);

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

struct scsi_scan_params {
	int root_lun_a;
	int root_lun_b;
	char scan_luns[SCSI_PARAM_MAX];
	char host[SCSI_HOST_MAX];
};

static int parse_lun_id(const char *str)
{
	char *end;
	long lun;

	if(!str || !*str)
		return SCSI_LUN_INVALID;

	errno = 0;
	lun = strtol(str, &end, 10);
	if(errno || end == str || *end != '\0' ||
			lun < 0 || lun > SCSI_SCAN_LUN_MAX)
		return SCSI_LUN_INVALID;

	return (int)lun;
}

static int parse_lun_range(char *range, int *start, int *end)
{
	char *dash;
	int lun_start;
	int lun_end;

	if(!range || !*range)
		return -EINVAL;

	dash = strchr(range, '-');
	if(dash) {
		*dash = '\0';
		lun_start = parse_lun_id(range);
		lun_end = parse_lun_id(dash + 1);
	} else {
		lun_start = parse_lun_id(range);
		lun_end = lun_start;
	}

	if(lun_start == SCSI_LUN_INVALID ||
			lun_end == SCSI_LUN_INVALID ||
			lun_start > lun_end)
		return -EINVAL;

	*start = lun_start;
	*end = lun_end;
	return 0;
}

static int read_scsi_scan_params(struct scsi_scan_params *params)
{
	char cmdline[4096] = {0};
	char *token;
	char *saveptr = NULL;
	int fd;
	int ret;

	memset(params, 0, sizeof(*params));
	params->root_lun_a = SCSI_LUN_INVALID;
	params->root_lun_b = SCSI_LUN_INVALID;

	fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
	if(fd < 0) {
		log_kmsg("scsi_scan: open cmdline failed: %d\n", errno);
		return -errno;
	}

	ret = read(fd, cmdline, sizeof(cmdline) - 1);
	safe_close(fd);
	if(ret < 0) {
		log_kmsg("scsi_scan: read cmdline failed: %d\n", errno);
		return -errno;
	}

	if(ret == (int)sizeof(cmdline) - 1)
		log_kmsg("scsi_scan: cmdline may be truncated (>= %zu bytes); "
			 "trailing params could be lost\n", sizeof(cmdline) - 1);

	token = strtok_r(cmdline, " \t\n", &saveptr);
	while(token) {
		const char root_lun_a_str[] = "early_ramdisk.root_lun_a=";
		const char root_lun_b_str[] = "early_ramdisk.root_lun_b=";
		const char scan_luns_str[] = "early_ramdisk.scan_luns=";
		const char scsi_host_str[] = "early_ramdisk.scsi_host=";
		int lun;

		if(!strncmp(token, root_lun_a_str, strlen(root_lun_a_str))) {
			lun = parse_lun_id(token + strlen(root_lun_a_str));
			if(lun == SCSI_LUN_INVALID)
				log_kmsg("scsi_scan: invalid %s\n", token);
			else
				params->root_lun_a = lun;
		} else if(!strncmp(token, root_lun_b_str, strlen(root_lun_b_str))) {
			lun = parse_lun_id(token + strlen(root_lun_b_str));
			if(lun == SCSI_LUN_INVALID)
				log_kmsg("scsi_scan: invalid %s\n", token);
			else
				params->root_lun_b = lun;
		} else if(!strncmp(token, scan_luns_str, strlen(scan_luns_str))) {
			strlcpy(params->scan_luns, token + strlen(scan_luns_str),
					sizeof(params->scan_luns));
		} else if(!strncmp(token, scsi_host_str, strlen(scsi_host_str))) {
			strlcpy(params->host, token + strlen(scsi_host_str),
					sizeof(params->host));
		}

		token = strtok_r(NULL, " \t\n", &saveptr);
	}

	return 0;
}

static int scsi_host_from_sysfs(char *host, size_t host_len)
{
	glob_t gl;
	char *base;
	int ret;

	memset(&gl, 0, sizeof(gl));
	ret = glob("/sys/class/scsi_host/host*", 0, NULL, &gl);
	if(ret || !gl.gl_pathc) {
		globfree(&gl);
		return -ENOENT;
	}

	base = strrchr(gl.gl_pathv[0], '/');
	if(!base) {
		globfree(&gl);
		return -EINVAL;
	}

	strlcpy(host, base + 1, host_len);
	globfree(&gl);
	return 0;
}

static int get_scsi_host(const struct scsi_scan_params *params,
		char *host, size_t host_len)
{
	if(params->host[0]) {
		strlcpy(host, params->host, host_len);
		return 0;
	}

	return scsi_host_from_sysfs(host, host_len);
}

static int scsi_host_id(const char *host)
{
	const char *id_str;
	char *end;
	long id;

	if(strncmp(host, "host", strlen("host")))
		return -EINVAL;

	id_str = host + strlen("host");
	if(!*id_str)
		return -EINVAL;

	errno = 0;
	id = strtol(id_str, &end, 10);
	if(errno || *end != '\0' || id < 0 || id > INT_MAX)
		return -EINVAL;

	return (int)id;
}

static int get_root_lun(const struct scsi_scan_params *params)
{
	const char *slot = get_slot_suffix();

	if(!slot) {
		log_kmsg("scsi_scan: slot suffix not available\n");
		return SCSI_LUN_INVALID;
	}

	if(!strncmp(slot, "_b", strlen("_b")))
		return params->root_lun_b;

	if(!strncmp(slot, "_a", strlen("_a")))
		return params->root_lun_a;

	log_kmsg("scsi_scan: unknown slot suffix %s\n", slot);
	return SCSI_LUN_INVALID;
}

static int write_scsi_scan_cmd(const char *host, int lun)
{
	char scan_path[96];
	char scan_cmd[16];
	int fd;
	int len;
	ssize_t ret;

	if(lun < 0 || lun > SCSI_SCAN_LUN_MAX)
		return -EINVAL;

	snprintf(scan_path, sizeof(scan_path),
			"/sys/class/scsi_host/%s/scan", host);
	fd = open(scan_path, O_WRONLY | O_CLOEXEC);
	if(fd < 0) {
		log_kmsg("scsi_scan: open %s failed: %d\n", scan_path, errno);
		return -errno;
	}

	len = snprintf(scan_cmd, sizeof(scan_cmd), "0 0 %d", lun);
	if(len < 0 || len >= (int)sizeof(scan_cmd)) {
		safe_close(fd);
		return -EINVAL;
	}
	ret = write(fd, scan_cmd, len);
	safe_close(fd);
	if(ret != len) {
		log_kmsg("scsi_scan: write %s failed: %d\n", scan_path, errno);
		return ret < 0 ? -errno : -EIO;
	}

	log_kmsg("scsi_scan: host %s lun %d\n", host, lun);
	return 0;
}

/*
 * scan_root_lun - trigger a SCSI scan of the root logical unit only.
 *
 * This is intentionally parameter-driven. Non-Nord targets are unchanged
 * because the tasklet returns without scanning unless the board cmdline
 * provides early_ramdisk.root_lun_a/root_lun_b.
 */
static int scan_root_lun(void *data)
{
	struct scsi_scan_params params;
	char host[SCSI_HOST_MAX] = {0};
	char wlun_glob[96];
	char blk_glob[96];
	char disk[32] = {0};
	char size_path[96];
	char part_glob[96];
	glob_t gl;
	int host_id;
	int root_lun;
	int ret;
	int count = 0;

	if(read_scsi_scan_params(&params))
		return 0;

	root_lun = get_root_lun(&params);
	if(root_lun == SCSI_LUN_INVALID)
		return 0;

	ret = get_scsi_host(&params, host, sizeof(host));
	if(ret) {
		log_kmsg("scan_root_lun: no scsi host found\n");
		return ret;
	}

	host_id = scsi_host_id(host);
	if(host_id < 0) {
		log_kmsg("scan_root_lun: invalid scsi host %s\n", host);
		return host_id;
	}

	snprintf(wlun_glob, sizeof(wlun_glob),
			"/sys/class/scsi_device/%d:0:0:*", host_id);

	for(; count < COND_CHECK_MAX; count++) {
		memset(&gl, 0, sizeof(gl));
		ret = glob(wlun_glob, 0, NULL, &gl);
		if(ret == 0 && gl.gl_pathc > 0) {
			globfree(&gl);
			break;
		}
		if(ret == 0)
			globfree(&gl);
		usleep(200);
	}

	if(count == COND_CHECK_MAX) {
		log_kmsg("scan_root_lun: timeout waiting for UFS host init\n");
		return -ETIMEDOUT;
	}

	if(count)
		log_info("scan_root_lun: waited %.1fms for UFS host init\n",
			 (count * 2) / 10.0);

	ret = write_scsi_scan_cmd(host, root_lun);
	if(ret)
		return ret;

	/*
	 * Wait for the root LUN's partition table to be parsed before returning.
	 *
	 * The scan write above is asynchronous: the kernel adds the SCSI device
	 * and reads its GPT in the background. This tasklet runs synchronously
	 * (module_run_tasklet) BEFORE @dm_create_tasklet and @wait_rootfs_tasklet,
	 * both of which resolve partitions via get_device_name() (blkid). If they
	 * run before the GPT is parsed, blkid can fall into a full device scan.
	 * Blocking here until the table is up keeps downstream rootfs resolution
	 * on the fast path.
	 *
	 * This wait uses sysfs only and derives the disk name from the device
	 * created by the scan, so it does not hardcode sda/sdg or read block
	 * devices while polling.
	 */
	snprintf(blk_glob, sizeof(blk_glob),
			"/sys/class/scsi_device/%d:0:0:%d/device/block/*",
			host_id, root_lun);

	for(count = 0; count < COND_CHECK_MAX; count++) {
		memset(&gl, 0, sizeof(gl));
		if(glob(blk_glob, 0, NULL, &gl) == 0 && gl.gl_pathc > 0) {
			char *base = strrchr(gl.gl_pathv[0], '/');
			if(base) {
				strlcpy(disk, base + 1, sizeof(disk));
				globfree(&gl);
				break;
			}
		}
		globfree(&gl);
		usleep(200);
	}

	if(disk[0] == '\0') {
		log_kmsg("scan_root_lun: root disk lun %d not enumerated\n",
			 root_lun);
		return 0;
	}

	snprintf(size_path, sizeof(size_path),
			"/sys/class/block/%s/size", disk);
	snprintf(part_glob, sizeof(part_glob),
			"/sys/class/block/%s/%s*", disk, disk);

	for(count = 0; count < COND_CHECK_MAX; count++) {
		unsigned long sz = 0;
		FILE *f = fopen(size_path, "r");
		int have_size = (f && fscanf(f, "%lu", &sz) == 1 && sz > 0);
		if(f)
			fclose(f);
		if(have_size) {
			memset(&gl, 0, sizeof(gl));
			if(glob(part_glob, 0, NULL, &gl) == 0 && gl.gl_pathc > 0) {
				globfree(&gl);
				break;
			}
			globfree(&gl);
		}
		usleep(200);
	}

	if(count == COND_CHECK_MAX)
		log_kmsg("scan_root_lun: timeout waiting for %s partitions\n",
			 disk);
	else if(count)
		log_info("scan_root_lun: %s ready after %.1fms\n",
			 disk, (count * 2) / 10.0);

	return 0;
}
TASKLET_EARLY_CALL("scan_root_lun_tasklet", scan_root_lun);

static int uni_overlayfs(char* path, char* machine, const char* chip)
{
	int ret = 0;
	char lower_dir[128];

	if (machine == NULL && chip == NULL) {
		log_kmsg("uni_overlayfs: No any machinae or chip is given!!!\n");
		return 0;
	}

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

	if (machine == NULL && chip == NULL) {
		log_kmsg("uni_bindfs: No any machinae or chip is given!!!\n");
		return 0;
	}

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
		case 690:
			socid_name = "nord";
			break;
		default:
			socid_name = "unknown";
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
		uni_overlayfs("/usr/bin", machine_name, socid_name);
		uni_overlayfs("/usr/lib", machine_name, socid_name);
		uni_overlayfs("/etc", machine_name, socid_name);

		// security driver load
		uni_bindfs("/etc/modules-load.d/security_load.conf", machine_name, "");
		uni_bindfs("/etc/fstab", machine_name, "");
	}

	return 0;
}
TASKLET_LATE_CALL("video_lib_unification_tasklet", video_lib_unification_func);

/*
 * scan_luns_func - scan board-provided UFS data LUNs after rootfs is up.
 *
 * Non-Nord targets are unchanged because this tasklet returns without scanning
 * unless early_ramdisk.scan_luns is present on the board cmdline. The active
 * root LUN is skipped because scan_root_lun_tasklet has already scanned it.
 */
static int scan_luns_func(void *data)
{
	struct scsi_scan_params params;
	char scan_luns[SCSI_PARAM_MAX];
	char host[SCSI_HOST_MAX] = {0};
	char *token;
	char *saveptr = NULL;
	int root_lun;
	int start;
	int end;
	int lun;
	int ret;

	if(read_scsi_scan_params(&params))
		return 0;

	if(!params.scan_luns[0])
		return 0;

	ret = get_scsi_host(&params, host, sizeof(host));
	if(ret) {
		log_kmsg("scan_luns: no scsi host found\n");
		return ret;
	}

	root_lun = get_root_lun(&params);
	if(root_lun == SCSI_LUN_INVALID)
		log_kmsg("scan_luns: root lun unknown, will not skip it\n");
	strlcpy(scan_luns, params.scan_luns, sizeof(scan_luns));

	token = strtok_r(scan_luns, ",", &saveptr);
	while(token) {
		if(parse_lun_range(token, &start, &end)) {
			log_kmsg("scan_luns: invalid lun range %s\n", token);
			token = strtok_r(NULL, ",", &saveptr);
			continue;
		}

		for(lun = start; lun <= end; lun++) {
			if(lun == root_lun)
				continue;

			ret = write_scsi_scan_cmd(host, lun);
			if(ret)
				log_kmsg("scan_luns: lun %d failed: %d\n", lun, ret);
		}

		token = strtok_r(NULL, ",", &saveptr);
	}

	return 0;
}
TASKLET_LATE_CALL("scan_luns_tasklet", scan_luns_func);
