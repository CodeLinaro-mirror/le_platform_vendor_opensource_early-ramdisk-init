/*
* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <libkmod.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <assert.h>
#include <sched.h>
#include <pthread.h>

#include "utils.h"
#include "thread_pool.h"

#define streq(a, b) (strcmp((a), (b)) == 0)
#define CONF_EARLY_DIR_PATH		"/etc/modules-load.f"
#define CONF_LATE_DIR_PATH		"/etc/modules-load.l"
#define MODULE_LINE_MAX			256
#define PATH_PAD			128
#define INIT_PATH_MAX			256
#define EARLY_FAST_MODE		(0x1)
#define LATE_FAST_MODE		(0x1 << 1)

struct conf_load_buffer {
	int len;
	char *buffer;
};

static pthread_mutex_t path_lock;

static void modules_load_kmod_log(void *data, int priority,
		const char *file, int line, const char *fn,
		const char *format, va_list args)
{
	vprintf(format, args);
}

static inline char *strim(char *s)
{
	size_t size = strlen(s);
	char *end;

	if(!size)
		return s;

	end = s + size -1;
	while(end >= s && (isspace(*end) || iscntrl(*end)))
		end--;
	*(end + 1) = '\0';

	return s;
}

static inline bool path_is_absolute(const char *p)
{
	assert(p != NULL);

	return p[0] == '/';
}

static char *module_get_abs_path(char *oldpath, int offset)
{
	int ret;
	char *newpath;
	struct utsname u;
	static char prefix_path[PATH_PAD] = "/lib/modules/";
	static const char *kerversion = NULL;
	static int len;

	pthread_mutex_lock(&path_lock);

	if(!kerversion) {
		if((ret = uname(&u)) < 0) {
			log_error("uname failed: %d\n", ret);
			pthread_mutex_unlock(&path_lock);
			return NULL;
		}
		kerversion = u.release;
		len = strlen(prefix_path);
		if((len + strlen(kerversion) + 1) > PATH_PAD) {
			log_error("kernel version too long!\n");
			kerversion = NULL;
			pthread_mutex_unlock(&path_lock);
			return NULL;
		}
		memcpy(prefix_path + len, kerversion, strlen(kerversion));
		len +=  strlen(kerversion);
		prefix_path[len] = '/';
		len++;
	}

	pthread_mutex_unlock(&path_lock);

	if(len > offset) {
		log_error("uname too long!\n");
		return NULL;
	}

	newpath = oldpath + offset - len;
	memcpy(newpath, prefix_path, len);
	return newpath;
}

static int module_insert_modules(struct kmod_ctx *ctx, char *module)
{
	int ret;
	struct kmod_module *kmod;
	char *path = module + PATH_PAD;
	unsigned int flags = 0;
	const char *opts = NULL;

	if(!path_is_absolute(path)) {
		if((path = module_get_abs_path(module, PATH_PAD)) == NULL) {
			return -EINVAL;
		}
	}

	log_info("insert: %s\n", path);

	ret = kmod_module_new_from_path(ctx, path, &kmod);
	if(ret) {
		log_error("Get kmod %s failed: %d\n", path, ret);
		return ret;
	}

	opts = kmod_module_get_options(kmod);
	ret = kmod_module_insert_module(kmod, flags, opts);
	if(ret < 0)
		log_error("could not insert module:%s:%d\n", path, ret);
	kmod_module_unref(kmod);

	return ret;
}

static int module_depend_check(struct kmod_ctx *ctx, char *module)
{
	char init_path[INIT_PATH_MAX] = {0};
	char buf[32] = {0};
	int ret = 0, fd = -1;
	int count = 0;
	struct kmod_module *kmod;
	char *path = module + PATH_PAD + 1;

	if(!path_is_absolute(path)) {
		if((path = module_get_abs_path(module, PATH_PAD + 1)) == NULL) {
			return -EINVAL;
		}
	}

	log_debug("depend check: %s\n", path);
	ret = kmod_module_new_from_path(ctx, path, &kmod);
	if(ret) {
		log_error("Get kmod failed: %d\n", ret);
		return ret;
	}

	snprintf(init_path, sizeof(init_path),
			"/sys/module/%s/initstate", kmod_module_get_name(kmod));

	log_debug("check state file: %s\n", init_path);
	while(count < COND_CHECK_MAX) {
		memset(buf, 0, sizeof(buf));
		fd = open(init_path, O_RDONLY | O_CLOEXEC);
		if(fd < 0) {
			log_debug("could not find init file: %s\n", init_path);
			count++;
			usleep(200);
			continue;
		}

		ret = read(fd, buf, sizeof(buf) - 1);
		if(ret < 0) {
			log_warn("could not read from %s: %d\n", init_path, ret);
			break;
		}
		ret = 0;

		strim(buf);
		if(streq(buf, "live"))
			break;
		else if(streq(buf, "coming") || streq(buf, "going")) {
			count++;
			close(fd);
			usleep(200);
			continue;
		} else {
			log_warn("%s: unknow state: %s\n", kmod_module_get_name(kmod), buf);
			count++;
			close(fd);
			usleep(200);
		}
	}
	if(fd >= 0)
		close(fd);

	if(count) {
		log_warn("depend check wait %s %.1f ms!!!\n", kmod_module_get_name(kmod),
				(count * 2) / 10.0);
		if(count == COND_CHECK_MAX)
			ret = -ETIMEDOUT;
	}

	return ret;
}

static inline int _module_set_cpu_affinity(cpu_set_t cpuset)
{
	pthread_t pid;

	pid = pthread_self();
	return pthread_setaffinity_np(pid, sizeof(cpuset), &cpuset);
}

static int module_set_cpu_affinity(char *arg, cpu_set_t *oldset)
{
	char *mask = arg + PATH_PAD + 1;
	const char name[] = {"mask="};
	int cpu = -1;
	cpu_set_t newset;
	pthread_t pid;
	int ret = -1;

	if(strncmp(mask, name, strlen(name)))
		return -EINVAL;

	mask += strlen(name);
	cpu = atoi(mask);

	if((cpu < 0) || (cpu >= get_nprocs_conf()))
		return -EINVAL;

	pid = pthread_self();
	ret = pthread_getaffinity_np(pid, sizeof(cpu_set_t), oldset);
	if(ret)
		return ret;

	CPU_ZERO(&newset);
	CPU_SET(cpu, &newset);

	return _module_set_cpu_affinity(newset);
}

static int module_clean_cpu_affinity(cpu_set_t old_mask)
{
	return _module_set_cpu_affinity(old_mask);
}

static int module_run_tasklet(char *task)
{
	char *task_name = task + PATH_PAD + 1;
	tasklet_func_t func;

	if(!task_name[0]) {
		log_warn("Empty tasklet name!\n");
		return -EINVAL;
	}

	strim(task_name);
	func = get_early_tasklet_from_string(task_name);
	if(!func) {
		log_warn("Tasklet %s not found!\n", task_name);
		return -EEXIST;
	}

	return func(NULL);
}

static void thread_modules_load(void *arg1, void *arg2)
{
	struct kmod_ctx *ctx = NULL;
	char *name = (char *)arg2;
	char line[MODULE_LINE_MAX + PATH_PAD] = {0};
	char *pline;
	FILE *f;
	int ret;
	int cpu_set_flag = 0;
	cpu_set_t oldset;

	log_info("config file: %s\n", name);

	/* Create ctx per thread */
	ctx = kmod_new(NULL, NULL);
	if(!ctx) {
		log_error("thread_modules_load:%s: Failed to allocate memory for kmod\n", name);
		return;
	}

	kmod_load_resources(ctx);
	kmod_set_log_fn(ctx, modules_load_kmod_log, NULL);

	f = fopen(name, "r");
	if(f == NULL) {
		log_error("%s open failed!\n", name);
		kmod_unref(ctx);
		return;
	}

	fseek(f, 0L, SEEK_END);
	if(ftell(f)<= 0) {
		log_error("%s: Wrong Size!\n", name);
		fclose(f);
		kmod_unref(ctx);
		return;
	}
	rewind(f);

	pline = line + PATH_PAD;
	while(fgets(pline, MODULE_LINE_MAX, f)) {
		strim(pline);

		switch(pline[0]) {
		case 'A' ... 'Z':
		case 'a' ... 'z':
		case '/':
			log_kmsg("insert: %s in config %s\n", pline, name);
			ret = module_insert_modules(ctx, line);
			log_kmsg("inserted: %s in config %s\n", pline, name);
			if(ret)
				log_error("insert %s failed: %d\n", pline, ret);
			break;
		case ':':
			pline[0] = 0;
			log_info("config: %s depend on %s\n", name, pline + 1);
			ret = module_depend_check(ctx, line);
			if(ret)
				log_warn("module depend check %s failed: %d\n", pline + 1, ret);
			break;
		case '>':
			ret = module_set_cpu_affinity(line, &oldset);
			if(ret)
				log_warn("module set cpu affinity %s failed: %d", pline, ret);
			cpu_set_flag = 1;
			break;
		case '@':
			pline[0] = 0;
			ret = module_run_tasklet(line);
			if(ret)
				log_warn("module run tasklet %s failed: %d\n", pline, ret);
			break;
		default:
			log_warn("Wrong format of %s!\n", pline);
			break;
		}
		memset(line, 0, sizeof(line));
	}

	if(cpu_set_flag)
		module_clean_cpu_affinity(oldset);

	log_info("config %s load done\n", name);
	fclose(f);
	kmod_unref(ctx);
}

int fast_modules_load(int load_mode)
{
	thread_pool_t *tp = NULL;
	struct dirent **conf_list;
	int i, conf_num;
	int ret = 0;
	int ncpus = 1;

	if(load_mode & EARLY_FAST_MODE) {
		ncpus = get_nprocs_conf();
		if(ncpus <= 0) {
			log_error("Can not get cpu numbers.\n");
			ncpus = 8;
		}
	}

	tp = thread_pool_init(ncpus);
	if(!tp) {
		log_error("Thread pool init failed!\n");
		ret = -ENOMEM;
		goto thread_pool_fail;
	}

	ret = pthread_mutex_init(&path_lock, NULL);
	if(ret != 0) {
		log_error("path_lock init failed!\n");
		goto thread_pool_fail;
	}

	conf_num = scandir(CONF_EARLY_DIR_PATH, &conf_list, NULL, alphasort);
	if(conf_num < 0) {
		log_error("%s scandir failed!\n", CONF_EARLY_DIR_PATH);
		goto conf_dir_error;
	}

	ret = chdir(CONF_EARLY_DIR_PATH);
	if(ret) {
		log_error("%s chdir failed\n", CONF_EARLY_DIR_PATH);
		goto chdir_error;
	}

	thread_pool_start(tp);
	/* set i = 2 to bypass . and .. */
	for(i = 2; i < conf_num; i++){
		if(conf_list[i]->d_type != DT_REG) {
			log_error("Unknow file type: %s\n", conf_list[i]->d_name);
			continue;
		}

		thread_pool_add_task(tp, thread_modules_load, NULL, conf_list[i]->d_name);
	}

	thread_pool_wait_finish(tp);

chdir_error:
	free(conf_list);
conf_dir_error:
	thread_pool_stop(tp);
	thread_pool_free(tp);
	pthread_mutex_destroy(&path_lock);
thread_pool_fail:
get_ncpu_fail:
	return ret;
}

static int late_tasklet_run(char *task, int load_mode)
{
	int status = 0;
	int ret = 0;
	char *task_name = task + 1;
	tasklet_func_t func;
	pid_t pid;

	if(!task_name[0]) {
		log_warn("Empty tasklet name!\n");
		return -EINVAL;
	}

	strim(task_name);
	func = get_late_tasklet_from_string(task_name);
	if(!func) {
		log_warn("Tasklet %s not found!\n", task_name);
		return -EEXIST;
	}

	pid = fork();
	if(pid < 0)
		log_kmsg("fork mount process failed\n");
	else if(pid == 0) {
		log_info("Run tasklet: %s\n", task_name);

		ret = func(NULL);

		exit(ret);
	}

	if(!(load_mode & LATE_FAST_MODE))
		waitpid(pid, &status, 0);

	return 0;
}

int late_tasklet_load_conf(char *buffer, int load_mode)
{
	char *line;
	char *saveptr;
	int ret;

	line = strtok_r(buffer, "\n", &saveptr);
	for(; line != NULL; line = strtok_r(NULL, "\n", &saveptr)) {
		strim(line);

		switch(line[0]) {
		case '@':
			ret = late_tasklet_run(line, load_mode);
			if(ret)
				log_warn("Late run tasklet %s failed: %d\n", line, ret);
			break;
		default:
			log_warn("Wrong format of %s!\n", line);
			break;
		}
	}

	return ret;
}

static struct conf_load_buffer *late_conf_buf;
static unsigned int late_conf_cnt;

int late_tasklet_load(int load_mode)
{
	int i;
	int ret = -ENOENT;
	struct conf_load_buffer *conf_buf;

	if(late_conf_cnt <=0) {
		log_kmsg("Late tasklet empty\n");
		return ret;
	}

	for(i = 0; i < late_conf_cnt; i++) {
		conf_buf = &late_conf_buf[i];
		if(!conf_buf || !conf_buf->buffer) {
			log_kmsg("late tasklet %d is empty\n");
			continue;
		}
		ret = late_tasklet_load_conf(conf_buf->buffer, load_mode);
		if(ret)
			log_warn("Late Run tasklet %d failed\n", i);
		free(conf_buf->buffer);
	}

	free(late_conf_buf);

	return ret;
}

static int late_tasklet_init_conf(char *conf_name,
		struct conf_load_buffer *conf_buf)
{
	FILE *f;
	int ret = 0, len;

	f = fopen(conf_name, "r");
	if(f == NULL) {
		log_error("%s open failed!\n", conf_name);
		return -ENOENT;
	}

	fseek(f, 0L, SEEK_END);
	if((len = ftell(f)) <= 0) {
		log_error("%s: Wrong Size!\n", conf_name);
		fclose(f);
		return -ENOENT;
	}
	rewind(f);

	conf_buf->len = len + 1;
	conf_buf->buffer = malloc(conf_buf->len);
	if(!conf_buf) {
		log_error("%s: Alloc buffer failed", conf_name);
	} else {
		memset(conf_buf->buffer, 0, conf_buf->len);
		ret = fread(conf_buf->buffer, 1, len, f);
	}

	fclose(f);

	return ret;
}

int late_tasklet_init(void)
{
	struct dirent **conf_list;
	int ret = 0;
	int i, conf_num;

	conf_num = scandir(CONF_LATE_DIR_PATH, &conf_list, NULL, alphasort);
	if(conf_num < 0) {
		log_error("%s scandir failed!\n", CONF_LATE_DIR_PATH);
		goto conf_dir_error;
	}

	ret = chdir(CONF_LATE_DIR_PATH);
	if(ret) {
		log_error("%s chdir failed\n", CONF_LATE_DIR_PATH);
		goto chdir_error;
	}

	late_conf_cnt = conf_num - 2;
	late_conf_buf = malloc(sizeof(struct conf_load_buffer) * late_conf_cnt);
	if(!late_conf_buf) {
		log_error("late tasklet init: Alloc conf_load_buffer failed");
		goto chdir_error;
	}
	memset(late_conf_buf, 0, sizeof(struct conf_load_buffer) * late_conf_cnt);

	for(i = 2; i < conf_num; i++){
		if(conf_list[i]->d_type != DT_REG) {
			log_error("Unknow file type: %s\n", conf_list[i]->d_name);
			continue;
		}

		ret = late_tasklet_init_conf(conf_list[i]->d_name, &late_conf_buf[i - 2]);
	}

chdir_error:
	free(conf_list);
conf_dir_error:
	return ret;
}
