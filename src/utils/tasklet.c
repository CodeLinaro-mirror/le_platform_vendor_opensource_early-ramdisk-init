/*
* Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
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

extern struct tasklet __tasklet_start[];
extern struct tasklet __tasklet_end[];

tasklet_func_t get_tasklet_from_string(char *name)
{
	struct tasklet *p;
	bool find = false;

	for(p = __tasklet_start; p < __tasklet_end; p++) {
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
