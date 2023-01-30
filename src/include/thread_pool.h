/*
* Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/

typedef struct thread_pool thread_pool_t;

thread_pool_t *thread_pool_init(int num_thread);

int thread_pool_start(thread_pool_t *tp);

int thread_pool_stop(thread_pool_t *tp);

int thread_pool_free(thread_pool_t *tp);

int thread_pool_add_task(thread_pool_t *tp, void (*func)(void *, void *),
		void *arg1, void *arg2);

int thread_pool_wait_finish(thread_pool_t *tp);
