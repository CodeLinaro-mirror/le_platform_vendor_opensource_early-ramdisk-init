/*
* Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/

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
#include <libkmod.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <assert.h>

#include "utils.h"
#include "thread_pool.h"

#define streq(a, b) (strcmp((a), (b)) == 0)
#define CONF_DIR_PATH		"/etc/modules-load.f"
#define MODULE_LINE_MAX			256
#define PATH_PAD			128
#define INIT_PATH_MAX			256
#define DEPEND_CHECK_MAX		10000 /* max wait 2 seconds */

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
	while(end >= s && isspace(*end))
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

	if(!kerversion) {
		if((ret = uname(&u)) < 0) {
			log_error("uname failed: %d\n", ret);
			return NULL;
		}
		kerversion = u.release;
		len = strlen(prefix_path);
		if((len + strlen(kerversion) + 1) > PATH_PAD) {
			log_error("kernel version too long!\n");
			kerversion = NULL;
			return NULL;
		}
		memcpy(prefix_path + len, kerversion, strlen(kerversion));
		len +=  strlen(kerversion);
		prefix_path[len] = '/';
		len++;
	}

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
		log_error("Get kmod failed: %d\n", ret);
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
	while(count < DEPEND_CHECK_MAX) {
		if(fd < 0) {
			fd = open(init_path, O_RDONLY | O_CLOEXEC);
			if(fd < 0) {
				log_debug("could not init file: %s\n", init_path);
				count++;
				usleep(2000);
				continue;
			}
		}

		ret = read(fd, buf, sizeof(buf));
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
			usleep(200);
			continue;
		} else {
			log_warn("%s: unknow state: %s\n", kmod_module_get_name(kmod), buf);
			count++;
			usleep(200);
		}
	}

	if(count) {
		log_warn("depend check wait %d times!!!\n", count);
		if(count == DEPEND_CHECK_MAX)
			ret = -ETIMEDOUT;
	}

	close(fd);
	return ret;
}

static void thread_modules_load(void *arg1, void *arg2)
{
	struct kmod_ctx *ctx = (struct kmod_ctx *)arg1;
	char *name = (char *)arg2;
	char line[MODULE_LINE_MAX + PATH_PAD] = {0};
	char *pline;
	int depend_check = 1;
	FILE *f;
	int ret;

	log_info("config file: %s\n", name);

	f = fopen(name, "r");
	if(f == NULL) {
		log_error("%s open failed!\n", name);
		return;
	}

	fseek(f, 0L, SEEK_END);
	if(ftell(f)<= 0) {
		log_error("%s: Wrong Size!\n", name);
		fclose(f);
		return;
	}
	rewind(f);

	pline = line + PATH_PAD;
	while(fgets(pline, MODULE_LINE_MAX, f)) {
		strim(pline);

		if(isalpha(pline[0]) || pline[0] == '/') {
			if(depend_check)
				depend_check = 0;
			ret = module_insert_modules(ctx, line);
			if(ret)
				log_error("insert %s failed: %d\n", pline, ret);
		} else if(pline[0] == ':') {
			if(!depend_check) {
				log_warn("Modules dependance must on head!\n");
				continue;
			}
			pline[0] = 0;
			ret = module_depend_check(ctx, line);
			if(ret)
				log_warn("module depend check %s failed: %d\n", pline + 1, ret);
		} else {
			 log_warn("Wrong format of %s!\n", pline);
			continue;
		}
		memset(line, 0, sizeof(line));
	}

	fclose(f);
}

int fast_modules_load(void)
{
	struct kmod_ctx *ctx = NULL;
	thread_pool_t *tp = NULL;
	struct dirent **conf_list;
	int i, conf_num;
	int ret = 0;
	int ncpus;

	ctx = kmod_new(NULL, NULL);
	if(!ctx) {
		log_error("Failed to allocate memory for kmod\n");
		return -ENOMEM;
	}

	kmod_load_resources(ctx);
	kmod_set_log_fn(ctx, modules_load_kmod_log, NULL);

	ncpus = get_nprocs_conf();
	if(ncpus <= 0) {
		log_error("Can not get cpu numbers.\n");
		ret = -EINVAL;
		goto get_ncpu_fail;
	}

	tp = thread_pool_init(ncpus);
	if(!tp) {
		log_error("Thread pool init failed!\n");
		ret = -ENOMEM;
		goto thread_pool_fail;
	}

	conf_num = scandir(CONF_DIR_PATH, &conf_list, NULL, alphasort);
	if(conf_num < 0) {
		log_error("%s scandir failed!\n", CONF_DIR_PATH);
		goto conf_dir_error;
	}

	ret = chdir(CONF_DIR_PATH);
	if(ret) {
		log_error("%s chdir failed\n", CONF_DIR_PATH);
		goto chdir_error;
	}

	thread_pool_start(tp);
	/* set i = 2 to bypass . and .. */
	for(i = 2; i < conf_num; i++){
		if(conf_list[i]->d_type != DT_REG) {
			log_error("Unknow file type: %s\n", conf_list[i]->d_name);
			continue;
		}

		thread_pool_add_task(tp, thread_modules_load, ctx, conf_list[i]->d_name);
	}

	thread_pool_wait_finish(tp);

chdir_error:
	free(conf_list);
conf_dir_error:
	thread_pool_stop(tp);
	thread_pool_free(tp);
thread_pool_fail:
get_ncpu_fail:
	kmod_unref(ctx);
	return ret;
}
