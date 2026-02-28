/*
* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/

SECTIONS
{
	. = ALIGN(16);
	__tasklet_early_start = .;
	rodata.data.early.tasklet : {
		KEEP(*(.data.early.tasklet))
	}
	__tasklet_early_end = .;
	__tasklet_late_start = .;
	rodata.data.late.tasklet : {
		KEEP(*(.data.late.tasklet))
	}
	__tasklet_late_end = .;
}
INSERT AFTER .rodata
