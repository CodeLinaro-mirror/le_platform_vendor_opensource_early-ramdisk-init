/*
* Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#ifndef _INIT_UTILS_H
#define _INIT_UTILS_H

extern size_t strlcpy(char *dst, const char *src, size_t siz);

typedef enum {
	LOG_DEBUG = 0,
	LOG_INFO,
	LOG_WARN,
	LOG_KERN,
	LOG_ERR,
	LOG_MAX,
} log_type_t;

int log_setup(const char *path);
void log_write(log_type_t level, const char *format, ...);
void log_close(void);

void log_kmsg(const char *format, ...);
#define log_error(...) log_write(LOG_ERR, __VA_ARGS__)
#define log_warn(...) log_write(LOG_WARN, __VA_ARGS__)
#define log_info(...) log_write(LOG_INFO, __VA_ARGS__)
#define log_debug(...)

void inline safe_free(char** p)
{
	if (*p)
		free(*p);
	*p = NULL;
	return;
}

void inline safe_close(int fd)
{
	if (fd > 0)
		close(fd);
	return;
}

#endif
