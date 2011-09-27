/*
 * Copyright (C) 2010 Philippe Gerum <rpm@xenomai.org>.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
 *
 * Timer object abstraction - Mercury core version.
 */

#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <memory.h>
#include <pthread.h>
#include <sched.h>
#include <limits.h>
#include "copperplate/threadobj.h"
#include "copperplate/timerobj.h"
#include "copperplate/clockobj.h"
#include "copperplate/debug.h"

static void timerobj_handler(union sigval sigval)
{
	struct timerobj *tmobj = sigval.sival_ptr;
	pthread_setspecific(threadobj_tskey, THREADOBJ_IRQCONTEXT);
	assert(tmobj->handler != NULL);
	tmobj->handler(tmobj);
}

int timerobj_init(struct timerobj *tmobj)
{
	struct sched_param param;
	pthread_attr_t thattr;
	struct sigevent evt;

	pthread_attr_init(&thattr);
	memset(&param, 0, sizeof(param));
	param.sched_priority = threadobj_irq_priority();
	pthread_attr_setinheritsched(&thattr, PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setschedpolicy(&thattr, SCHED_FIFO);
	pthread_attr_setschedparam(&thattr, &param);
	pthread_attr_setstacksize(&thattr, PTHREAD_STACK_MIN * 16);

	/*
	 * XXX: We need a threaded handler so that we may invoke core
	 * async-unsafe services from there (e.g. syncobj post
	 * routines are not async-safe, but the higher layers may
	 * invoke them from a timer handler).
	 */
	evt.sigev_notify = SIGEV_THREAD;
	evt.sigev_value.sival_ptr = tmobj;
	evt.sigev_notify_function = timerobj_handler;
	evt.sigev_notify_attributes = &thattr;

	tmobj->handler = NULL;

	if (__STD(timer_create(CLOCK_COPPERPLATE, &evt, &tmobj->timer)))
		return __bt(-errno);

	return 0;
}

int timerobj_destroy(struct timerobj *tmobj)
{
	if (__STD(timer_delete(tmobj->timer)))
		return __bt(-errno);

	return 0;
}

int timerobj_start(struct timerobj *tmobj,
		   void (*handler)(struct timerobj *tmobj),
		   struct itimerspec *it)
{
	tmobj->handler = handler;
	membar();

	if (__STD(timer_settime(tmobj->timer, TIMER_ABSTIME, it, NULL)))
		return __bt(-errno);

	return 0;
}

static const struct itimerspec itimer_stop = {
	.it_value = {
		.tv_sec = 0,
		.tv_nsec = 0,
	},
	.it_interval = {
		.tv_sec = 0,
		.tv_nsec = 0,
	},
};

int timerobj_stop(struct timerobj *tmobj)
{
	if (__STD(timer_settime(tmobj->timer, 0, &itimer_stop, NULL)))
		return __bt(-errno);

	barrier();
	tmobj->handler = NULL;

	return 0;
}

int timerobj_pkg_init(void)
{
	return 0;
}
