/*
* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <blkid/blkid.h>
#include <ctype.h>
#include <glob.h>

#include "utils.h"

/*
 * resolve_partition_sysfs - resolve "PARTLABEL=<name>" or "PARTUUID=<uuid>"
 * to /dev/<node> via sysfs uevent matching.  No blkid, no UFS device reads.
 *
 * PARTLABEL matches the uevent PARTNAME= line (case-sensitive); PARTUUID
 * matches the uevent PARTUUID= line (case-insensitive, as the kernel emits
 * lowercase hex while a cmdline UUID may be upper-case).
 *
 * Returns malloc'd "/dev/<devname>" the caller must free(), or NULL on miss.
 * Callers must fall back to get_device_name() on NULL (this only handles
 * PARTLABEL/PARTUUID for already-enumerated partitions; LABEL=/UUID= are
 * filesystem-level and still require blkid).
 */
char *resolve_partition_sysfs(const char *token)
{
	static const char label_prefix[] = "PARTLABEL=";
	static const char uuid_prefix[] = "PARTUUID=";
	const char *want;		/* value we are matching on            */
	const char *uevent_key;		/* uevent line key to compare against   */
	size_t uevent_key_len;
	bool case_insensitive;
	DIR *d;
	struct dirent *e;
	char *result = NULL;
	size_t result_size = 0;

	if (strncmp(token, label_prefix, sizeof(label_prefix) - 1) == 0) {
		want = token + sizeof(label_prefix) - 1;
		uevent_key = "PARTNAME=";
		uevent_key_len = strlen("PARTNAME=");
		case_insensitive = false;
	} else if (strncmp(token, uuid_prefix, sizeof(uuid_prefix) - 1) == 0) {
		want = token + sizeof(uuid_prefix) - 1;
		uevent_key = "PARTUUID=";
		uevent_key_len = strlen("PARTUUID=");
		case_insensitive = true;
	} else {
		return NULL;
	}

	d = opendir("/sys/class/block");
	if (!d)
		return NULL;

	while ((e = readdir(d)) && !result) {
		char upath[256];
		char line[256];
		FILE *f;

		if (e->d_name[0] == '.')
			continue;

		snprintf(upath, sizeof(upath),
			 "/sys/class/block/%s/uevent", e->d_name);
		f = fopen(upath, "r");
		if (!f)
			continue;

		while (fgets(line, sizeof(line), f)) {
			size_t len = strlen(line);
			const char *val;

			if (len > 0 && line[len - 1] == '\n') {
				line[len - 1] = '\0';
			} else if (len == sizeof(line) - 1) {
				/*
				 * Line longer than the buffer: fgets stopped
				 * mid-line with no newline. Comparing it would
				 * risk a false match, so drain the rest of this
				 * line and skip it.
				 */
				int c;
				while ((c = fgetc(f)) != '\n' && c != EOF)
					;
				continue;
			}
			if (strncmp(line, uevent_key, uevent_key_len) != 0)
				continue;

			val = line + uevent_key_len;
			if (case_insensitive ? (strcasecmp(val, want) == 0)
					     : (strcmp(val, want) == 0)) {
				result_size = strlen("/dev/") +
						strlen(e->d_name) + 1;
				result = malloc(result_size);
				if (result)
					snprintf(result, result_size, "/dev/%s", e->d_name);
				break;
			}
		}
		fclose(f);
	}
	closedir(d);
	return result;
}

//Put right system_* info here can save aroung ~60ms bootkpi
static const char *rootfs_patterns[] = {"/dev/sd*42", "/dev/sd*8", "/dev/sd*22", "/dev/sd*6", "/dev/sd*4", "/dev/sd*", "/dev/mmcblk*p21", "/dev/mmcblk*"};

static bool find_the_device(char* devname, const char* token)
{
	bool ret = false;
	blkid_probe probe = blkid_new_probe();
	if (!probe)
	{
		log_kmsg("blkid_new_probe failed");
		goto probe_error;
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
	const char* val;
	if (!strncmp(token, "PARTUUID", strlen("PARTUUID")) &&
			!blkid_probe_lookup_value (probe, "PART_ENTRY_UUID", &val, NULL) &&
			!strncmp (val, (token + sizeof ("PARTUUID")), strlen(val)))
		ret = true;
	else if (!strncmp(token, "PARTLABEL", strlen("PARTLABEL")) &&
			!blkid_probe_lookup_value (probe, "PART_ENTRY_NAME", &val, NULL) &&
			!strncmp (val, (token + sizeof ("PARTLABEL")), strlen(val)))
		ret = true;
out:
        safe_close(fd);
probe_error:
	if (probe)
		blkid_free_probe(probe);
	return ret;
}

char* get_device_name(const char* token)
{
	char* dev = NULL;
	if (!strncmp(token, "PARTUUID", strlen("PARTUUID")) ||
		!strncmp(token, "PARTLABEL", strlen("PARTLABEL"))) {
		glob_t block_device_list;

		for (size_t  i = 0; i < (sizeof(rootfs_patterns)/sizeof(rootfs_patterns[0])); ++i)
			glob(rootfs_patterns[i],i ? GLOB_APPEND : 0 , NULL, &block_device_list);
		for (size_t i = 0; i < block_device_list.gl_pathc; ++i) {
			if (find_the_device(block_device_list.gl_pathv[i], token)) {
				dev = strdup(block_device_list.gl_pathv[i]);
				break;
			}
		}
		globfree(&block_device_list);
	}

	if(!dev) {
		//Slow path
		dev = blkid_get_devname(NULL, token, NULL);
	}
	return dev;
}
