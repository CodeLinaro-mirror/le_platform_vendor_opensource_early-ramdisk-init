/*
* Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/

SECTIONS
{
	. = ALIGN(16);
	__tasklet_start = .;
	rodata.data.tasklet : {
		KEEP(*(.data.tasklet))
	}
	__tasklet_end = .;
}
INSERT AFTER .rodata
