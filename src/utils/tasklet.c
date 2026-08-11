/*
* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>

#include "utils.h"

extern const struct tasklet __tasklet_early_start[];
extern const struct tasklet __tasklet_early_end[];
extern const struct tasklet __tasklet_late_start[];
extern const struct tasklet __tasklet_late_end[];

static inline tasklet_func_t get_tasklet_from_string(char *name,
		const struct tasklet *task_start, const struct tasklet *task_end)
{
	const struct tasklet *p;
	bool find = false;

	for(p = task_start; p < task_end; p++) {
		if(!p->name)
			continue;

		if(!strcmp(name, p->name)) {
			find = true;
			break;
		}
	}

	if(find)
		return p->func;
	else
		return NULL;
}

tasklet_func_t get_early_tasklet_from_string(char *name)
{
	return get_tasklet_from_string(name, __tasklet_early_start, __tasklet_early_end);
}

tasklet_func_t get_late_tasklet_from_string(char *name)
{
	return get_tasklet_from_string(name, __tasklet_late_start, __tasklet_late_end);
}
