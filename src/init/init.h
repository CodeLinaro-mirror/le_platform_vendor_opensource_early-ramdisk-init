/*
* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#ifndef _INIT_H
#define _INIT_H

int fast_modules_load(int);
int late_tasklet_load(int);
int late_tasklet_init(void);

int mount_setup(void);
void mount_unsetup(void);

#endif
