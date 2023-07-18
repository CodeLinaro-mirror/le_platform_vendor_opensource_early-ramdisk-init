/*
* Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "utils.h"

static int log_fd = 0;
#define MSG_LEN					128
#define KMSG_HEADER				"early-ramdiskinit: "

void log_kmsg(const char *format, ...)
{
	char msg[MSG_LEN] = {KMSG_HEADER};
	char *pstr = msg + strlen(KMSG_HEADER);
	va_list va;
	int fd;

	va_start(va, format);
	vsnprintf(pstr, MSG_LEN - strlen(KMSG_HEADER), format, va);
	va_end(va);

	log_write(LOG_KERN, pstr);

	if(strlen(msg) > MSG_LEN)
		return;

	fd = open("/dev/kmsg", O_WRONLY|O_NOCTTY|O_CLOEXEC);
	if(fd > 0) {
		if(write(fd, msg, strlen(msg)) < 0)
			perror("write kmsg error:");
	}
	safe_close(fd);
}

int log_setup(const char *path)
{
	log_fd = open(path, O_RDWR | O_CREAT, 0644);
	if(log_fd < 0) {
		return log_fd;
	}
	return 0;
}

void log_write(log_type_t level, const char *format, ...)
{
	const char log_type[LOG_MAX][8] =
		{"DEBUG:", "INFO:", "WARN:", "KERN:", "ERR:"};
	char msg[MSG_LEN] = {0};
	va_list va;
	int t_len;

	if(log_fd < 0)
		return;

#ifdef DEBUG
	{
		int fd = -1;
		fd = open("/dev/kmsg", O_WRONLY|O_NOCTTY|O_CLOEXEC);
		if(fd > 0) {
			if(write(fd, msg, strlen(msg)) < 0)
				perror("write kmsg error:");
		}
		safe_close(fd);
	}
#endif

	t_len = strlen(log_type[level]);

	snprintf(msg, t_len + 1, "%s", log_type[level]);
	va_start(va, format);
	vsnprintf(msg + t_len, MSG_LEN - t_len, format, va);
	va_end(va);

	if(write(log_fd, msg, strlen(msg)) < 0)
		perror("early-ramdisk-init: write log fail");
}

void log_close(void)
{
	if(log_fd > 0)
		close(log_fd);
	log_fd = -1;
}

