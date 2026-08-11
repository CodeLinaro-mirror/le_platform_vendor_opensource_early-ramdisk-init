/*
* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/

SECTIONS
{
	. = ALIGN(16);
	__tasklet_early_start = .;
	early_tasklet : {
		KEEP(*(early_tasklet))
	}
	__tasklet_early_end = .;

	. = ALIGN(16);
	__tasklet_late_start = .;
	late_tasklet : {
		KEEP(*(late_tasklet))
	}
	__tasklet_late_end = .;
}
INSERT AFTER .data.rel.ro
