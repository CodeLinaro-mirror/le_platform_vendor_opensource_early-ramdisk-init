/*
* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <blkid/blkid.h>
#include <ctype.h>
#include <glob.h>

#include "utils.h"

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
