/*
* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#ifndef _INIT_UTILS_H
#define _INIT_UTILS_H

#define COND_CHECK_MAX		10000 /* max wait 2 seconds */

int wait_rootfs_tasklet(void *data);
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
#define log_error(...) log_kmsg(__VA_ARGS__)
#define log_warn(...) log_write(LOG_WARN, __VA_ARGS__)
#define log_info(...) log_write(LOG_INFO, __VA_ARGS__)
#define log_debug(...)

char* get_slot_suffix(void);

typedef int (*tasklet_func_t)(void *);

struct tasklet {
	char *name;
	tasklet_func_t func;
};

#define TASKLET_EARLY_CALL(_name, _func) \
	struct tasklet _func##_tasklet __attribute__((unused)) \
	__attribute__((section(".data.early.tasklet"))) = { _name, _func};

#define TASKLET_LATE_CALL(_name, _func) \
	struct tasklet _func##_tasklet __attribute__((unused)) \
	__attribute__((section(".data.late.tasklet"))) = { _name, _func};

tasklet_func_t get_early_tasklet_from_string(char *name);
tasklet_func_t get_late_tasklet_from_string(char *name);

struct MountPoint {
	const char *name;
	const char *where;
	const char *type;
	unsigned long flags;
	const char *options;
};
//extern struct MountPoint mount_table[16];
char* get_device_name(const char* token);

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
