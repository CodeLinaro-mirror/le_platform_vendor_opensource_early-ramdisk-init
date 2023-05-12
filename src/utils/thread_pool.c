/*
* Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/queue.h>

#include "thread_pool.h"
#include "utils.h"

#define THREAD_POOL_LOOP_PERIOD				(5 * 1000 * 1000) //5ms

struct thread_task {
	void (*func)(void *, void *);
	void *arg1;
	void *arg2;
	CIRCLEQ_ENTRY(thread_task) entry;
};

enum thread_pool_state {
	THREAD_POOL_FREE,
	THREAD_POOL_STOP,
	THREAD_POOL_INIT,
	THREAD_POOL_READY,
	THREAD_POOL_RUN,
};

struct thread_pool {
	pthread_mutex_t lock;
	pthread_cond_t notify;
	volatile int num_total;
	volatile int num_tasks;
	pthread_t *thread;
	CIRCLEQ_HEAD(taskq, thread_task) taskq;
	enum thread_pool_state state;

};

static void *thread_pool_loop(void *data);

static void inline thread_timeout_value(struct timespec *ts)
{
	clock_gettime(CLOCK_REALTIME, ts);
	ts->tv_nsec += THREAD_POOL_LOOP_PERIOD;
	if(ts->tv_nsec >= 1000000000) {
		ts->tv_sec++;
		ts->tv_nsec -= 1000000000;
	}
}

static void thread_pool_destory(thread_pool_t *tp)
{
	int i;

	tp->state = THREAD_POOL_STOP;

	for(i = 0; i < tp->num_total; i++) {
		if(tp->thread[i])
			pthread_cancel(tp->thread[i]);
	}

	thread_pool_free(tp);
}

thread_pool_t *thread_pool_init(int num_thread)
{
	thread_pool_t *tp;
	int i;

	if(num_thread < 0)
		return NULL;

	tp = malloc(sizeof(*tp));
	if(!tp)
		return NULL;
	memset(tp, 0, sizeof(*tp));

	tp->thread = malloc(num_thread * sizeof(pthread_t));
	if(!tp->thread)
		goto malloc_fail;
	memset(tp->thread, 0, num_thread * sizeof(pthread_t));

	tp->state = THREAD_POOL_INIT;

	CIRCLEQ_INIT(&tp->taskq);

	pthread_mutex_init(&tp->lock, NULL);
	pthread_cond_init(&tp->notify, NULL);

	for(i = 0; i < num_thread; i++)
	{
		if(pthread_create(&(tp->thread[i]), NULL, thread_pool_loop, (void *)tp)) {
			log_error("Thread pool: create thread %d fail!\n", i);
			thread_pool_destory(tp);
			return NULL;
		}
	}
	tp->state = THREAD_POOL_READY;

	tp->num_total = num_thread;

	return tp;

malloc_fail:
	free(tp);
	return NULL;
}

int thread_pool_start(thread_pool_t *tp)
{
	tp->state = THREAD_POOL_RUN;
	pthread_cond_broadcast(&tp->notify);

	return 0;
}

int thread_pool_stop(thread_pool_t *tp)
{
	tp->state = THREAD_POOL_STOP;
	pthread_cond_broadcast(&tp->notify);

	return 0;
}

int thread_pool_wait_finish(thread_pool_t *tp)
{
	struct timespec time_wait;
	while(tp->num_tasks) {
		thread_timeout_value(&time_wait);
		pthread_mutex_lock(&tp->lock);
		pthread_cond_timedwait(&tp->notify, &tp->lock, &time_wait);
		pthread_mutex_unlock(&tp->lock);
	}

	return 0;
}

int thread_pool_free(thread_pool_t *tp)
{
	int i;

	tp->state = THREAD_POOL_FREE;
	for(i = 0; i < tp->num_total; i++)
		pthread_join(tp->thread[i], NULL);

	pthread_cond_destroy(&tp->notify);
	pthread_mutex_destroy(&tp->lock);

	free(tp->thread);
	free(tp);

	return 0;

}

int thread_pool_add_task(thread_pool_t *tp, void (*func)(void *, void *),
		void *arg1, void *arg2)
{
	struct thread_task *task;

	task = malloc(sizeof(*task));
	if(!task) {
		log_error("Thread Pool: allocate task failed!\n");
		return -ENOMEM;
	}
	memset(task, 0, sizeof(*task));

	pthread_mutex_lock(&tp->lock);
	task->func = func;
	task->arg1 = arg1;
	task->arg2 = arg2;
	CIRCLEQ_INSERT_TAIL(&tp->taskq, task, entry);
	pthread_cond_signal(&tp->notify);
	tp->num_tasks++;
	pthread_mutex_unlock(&tp->lock);

	return 0;
}

static void *thread_pool_loop(void *data)
{
	thread_pool_t *tp = (thread_pool_t *)data;
	struct thread_task *task;
	struct timespec time_wait;

	while(tp->state > THREAD_POOL_STOP) {

		if(tp->state != THREAD_POOL_RUN) {
			pthread_mutex_lock(&tp->lock);
			pthread_cond_wait(&tp->notify, &tp->lock);
			pthread_mutex_unlock(&tp->lock);
			continue;
		}

		thread_timeout_value(&time_wait);
		pthread_mutex_lock(&tp->lock);
		pthread_cond_timedwait(&tp->notify, &tp->lock, &time_wait);

		if(CIRCLEQ_EMPTY(&tp->taskq)) {
			pthread_mutex_unlock(&tp->lock);
			continue;
		}

		task = CIRCLEQ_FIRST(&tp->taskq);
		CIRCLEQ_REMOVE(&tp->taskq, task, entry);
		pthread_mutex_unlock(&tp->lock);

		task->func(task->arg1, task->arg2);
		free(task);

		pthread_mutex_lock(&tp->lock);
		tp->num_tasks--;
		if(!tp->num_tasks) {
			pthread_cond_broadcast(&tp->notify);
		}
		pthread_mutex_unlock(&tp->lock);
	}

	pthread_exit(NULL);
	return NULL;
}

