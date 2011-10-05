/*
 * Copyright (C) 2008 Philippe Gerum <rpm@xenomai.org>.
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
 */

#include <stdio.h>
#include <memory.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>
#include <fcntl.h>
#include <sched.h>
#include <limits.h>
#include "taskLib.h"
#include "tickLib.h"
#include <copperplate/panic.h>
#include <copperplate/heapobj.h>
#include <copperplate/threadobj.h>
#include <copperplate/syncobj.h>
#include <copperplate/cluster.h>
#include <vxworks/errnoLib.h>

struct cluster wind_task_table;

static unsigned long anon_tids;

static struct wind_task *find_wind_task(TASK_ID tid)
{
	struct WIND_TCB *tcb = mainheap_deref(tid, struct WIND_TCB);
	struct wind_task *task;

	/*
	 * Best-effort to validate a TCB pointer the cheap way,
	 * without relying on any syscall.
	 */
	if (tcb == NULL || ((uintptr_t)tcb & (sizeof(uintptr_t)-1)) != 0)
		return NULL;

	task = tcb->opaque;
	if (task == NULL || ((uintptr_t)task & (sizeof(uintptr_t)-1)) != 0)
		return NULL;

	if (threadobj_get_magic(&task->thobj) != task_magic)
		return NULL;

	return task;
}

static struct wind_task *find_wind_task_or_self(TASK_ID tid)
{
	if (tid)
		return find_wind_task(tid);

	return wind_task_current();
}

struct wind_task *get_wind_task(TASK_ID tid)
{
	struct wind_task *task = find_wind_task(tid);

	/*
	 * Grab the task lock, assuming that the task might have been
	 * deleted, and/or maybe we have been lucky, and some random
	 * opaque pointer might lead us to something which is laid in
	 * valid memory but certainly not to a task object. Last
	 * chance is pthread_mutex_lock() in threadobj_lock()
	 * detecting a wrong mutex kind and bailing out.
	 *
	 * XXX: threadobj_lock() disables cancellability for the
	 * caller upon success, until the lock is dropped in
	 * threadobj_unlock(), so there is no way it may vanish while
	 * holding the lock. Therefore we need no cleanup handler
	 * here.
	 */
	if (task == NULL || threadobj_lock(&task->thobj) == -EINVAL)
		return NULL;

	/* Check the magic word again, while we hold the lock. */
	if (threadobj_get_magic(&task->thobj) != task_magic) {
		threadobj_unlock(&task->thobj);
		return NULL;
	}

	return task;
}

struct wind_task *get_wind_task_or_self(TASK_ID tid)
{
	struct wind_task *current;

	if (tid)
		return get_wind_task(tid);

	current = wind_task_current();
	if (current == NULL)
		return NULL;

	/* This one might block but can't fail, it is ours. */
	threadobj_lock(&current->thobj);

	return current;
}

void put_wind_task(struct wind_task *task)
{
	threadobj_unlock(&task->thobj);
}

static void task_finalizer(struct threadobj *thobj)
{
	struct wind_task *task = container_of(thobj, struct wind_task, thobj);

	task->tcb->status |= WIND_DEAD;
	cluster_delobj(&wind_task_table, &task->cobj);
	registry_destroy_file(&task->fsobj);
	__RT(pthread_mutex_destroy(&task->safelock));
	threadobj_destroy(&task->thobj);

	xnfree(task);
}

/*
 * XXX: Wait and suspend hooks run on behalf of the target task, so no
 * lock is needed to prevent unexpected exit while accessing their
 * TCB.
 */
static void task_wait_hook(struct threadobj *thobj, int status)
{
	struct wind_task *task = container_of(thobj, struct wind_task, thobj);

	if (status & SYNCOBJ_BLOCK)
		task->tcb->status |= WIND_PEND;
	else
		task->tcb->status &= ~WIND_PEND;
}

static void task_suspend_hook(struct threadobj *thobj, int status)
{
	struct wind_task *task = container_of(thobj, struct wind_task, thobj);

	if (status & THREADOBJ_SUSPEND)
		task->tcb->status |= WIND_SUSPEND;
	else
		task->tcb->status &= ~WIND_SUSPEND;
}

#ifdef CONFIG_XENO_REGISTRY

static inline char *task_decode_status(struct wind_task *task, char *buf)
{
	int status;

	*buf = '\0';
	status = threadobj_get_status(&task->thobj);
	if (status & THREADOBJ_SCHEDLOCK)
		strcat(buf, "+sched_lock");
	if (status & THREADOBJ_ROUNDROBIN)
		strcat(buf, "+sched_rr");

	status = task->tcb->status;
	if (status == WIND_READY)
		strcat(buf, "+ready");
	else {
		if (status & WIND_SUSPEND)
			strcat(buf, "+suspended");
		if (status & WIND_PEND)
			strcat(buf, "+pending");
		if (status & WIND_DELAY)
			strcat(buf, "+delayed");
	}

	return buf + 1;
}

static size_t task_registry_read(struct fsobj *fsobj,
				 char *buf, size_t size, off_t offset)
{
	struct wind_task *task;
	char sbuf[64];
	size_t len;

	task = container_of(fsobj, struct wind_task, fsobj);
	len =  sprintf(buf,       "name       = %s\n", task->name);
	len += sprintf(buf + len, "errno      = %d\n", threadobj_get_errno(&task->thobj));
	len += sprintf(buf + len, "status     = %s\n", task_decode_status(task, sbuf));
	len += sprintf(buf + len, "priority   = %d\n", wind_task_get_priority(task));
	len += sprintf(buf + len, "lock_depth = %d\n", threadobj_get_lockdepth(&task->thobj));

	return len;
}

static struct registry_operations registry_ops = {
	.read	= task_registry_read
};

#else

static struct registry_operations registry_ops;

#endif /* CONFIG_XENO_REGISTRY */

static void *task_trampoline(void *arg)
{
	struct wind_task *task = arg;
	struct wind_task_args *args = &task->args;
	struct service svc;
	int ret;

	ret = __bt(threadobj_prologue(&task->thobj, task->name));
	if (ret)
		goto done;

	COPPERPLATE_PROTECT(svc);

	ret = __bt(registry_add_file(&task->fsobj, O_RDONLY,
				     "/vxworks/tasks/%s", task->name));
	if (ret)
		warning("failed to export task %s to registry",
			task->name);

	COPPERPLATE_UNPROTECT(svc);

	/* Wait for someone to run taskActivate() upon us. */
	threadobj_wait_start(&task->thobj);

	args->entry(args->arg0, args->arg1, args->arg2, args->arg3,
		    args->arg4, args->arg5, args->arg6, args->arg7,
		    args->arg8, args->arg9);
done:
	threadobj_lock(&task->thobj);
	threadobj_set_magic(&task->thobj, ~task_magic);
	threadobj_unlock(&task->thobj);

	pthread_exit((void *)(long)ret);
}

/*
 * By default, VxWorks priorities are mapped 1:1 to SCHED_RT
 * levels. SCHED_RT is SCHED_COBALT in dual kernel mode, or SCHED_FIFO
 * when running over the Mercury core. We allow up to 257 priority
 * levels over Cobalt when running in primary mode, 99 over the
 * regular glibc's POSIX interface.
 *
 * NOTE: in dual kernel mode, a thread transitioning to secondary mode
 * has its priority ceiled to 99 in the SCHED_FIFO class.
 *
 * The application code may override the routines doing the priority
 * mappings from VxWorks to SCHED_RT (normalize) and conversely
 * (denormalize). The bottom line is that normalized priorities should
 * be in the range [ 1 .. sched_get_priority_max(SCHED_RT) - 1 ]
 * inclusive.
 */

__attribute__ ((weak))
int wind_task_normalize_priority(int wind_prio)
{
	/*
	 * SCHED_RT priorities are always 1-based regardless of the
	 * underlying real-time core. We remap the lowest VxWorks
	 * priority to the lowest available level in the SCHED_RT
	 * policy.
	 */
	if (wind_prio > threadobj_high_prio - 1)
		panic("current implementation restricts VxWorks "
		      "priority levels to range [%d..0]",
		      threadobj_high_prio - 1);

	/* Map a VxWorks priority level to a SCHED_RT one. */
	return threadobj_high_prio - wind_prio - 1;
}

__attribute__ ((weak))
int wind_task_denormalize_priority(int core_prio)
{
	/* Map a SCHED_RT priority level to a VxWorks one. */
	return threadobj_high_prio - core_prio - 1;
}

static int check_task_priority(u_long wind_prio, int *core_prio)
{
	if (wind_prio < 0 || wind_prio > 255) /* In theory. */
		return S_taskLib_ILLEGAL_PRIORITY;

	*core_prio = wind_task_normalize_priority(wind_prio);

	return OK;
}

static STATUS __taskInit(struct wind_task *task,
			 struct WIND_TCB *tcb, const char *name,
			 int prio, int flags, FUNCPTR entry, int stacksize)
{
	struct threadobj_init_data idata;
	pthread_mutexattr_t mattr;
	struct sched_param param;
	pthread_attr_t thattr;
	int ret, cprio;

	ret = check_task_priority(prio, &cprio);
	if (ret) {
		errno = ret;
		return ERROR;
	}

	if (name == NULL || *name == '\0')
		sprintf(task->name, "t%lu", ++anon_tids);
	else {
		strncpy(task->name, name, sizeof(task->name));
		task->name[sizeof(task->name) - 1] = '\0';
	}

	__RT(pthread_mutexattr_init(&mattr));
	__RT(pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE));
	__RT(pthread_mutexattr_setprotocol(&mattr, PTHREAD_PRIO_INHERIT));
	__RT(pthread_mutexattr_setpshared(&mattr, mutex_scope_attribute));
	__RT(pthread_mutex_init(&task->safelock, &mattr));
	__RT(pthread_mutexattr_destroy(&mattr));

	task->tcb = tcb;
	tcb->opaque = task;
	/*
	 * CAUTION: tcb->status in only modified by the owner task
	 * (see suspend/resume hooks), or when such task is guaranteed
	 * not to be running, e.g. in taskActivate(). So we do NOT
	 * take any lock specifically for updating it. However, we
	 * know that a memory barrier will be issued shortly after
	 * such updates because of other locking being in effect, so
	 * we don't explicitely have to provide for it.
	 */
	tcb->status = WIND_SUSPEND;
	tcb->safeCnt = 0;
	tcb->flags = flags;
	tcb->entry = entry;

	pthread_attr_init(&thattr);

	if (stacksize == 0)
		stacksize = PTHREAD_STACK_MIN * 4;
	else if (stacksize < PTHREAD_STACK_MIN)
		stacksize = PTHREAD_STACK_MIN;

	memset(&param, 0, sizeof(param));
	param.sched_priority = cprio;
	pthread_attr_setinheritsched(&thattr, PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setschedpolicy(&thattr, SCHED_RT);
	pthread_attr_setschedparam(&thattr, &param);
	pthread_attr_setstacksize(&thattr, stacksize);
	pthread_attr_setscope(&thattr, thread_scope_attribute);
	pthread_attr_setdetachstate(&thattr, PTHREAD_CREATE_JOINABLE);

	idata.magic = task_magic;
	idata.wait_hook = task_wait_hook;
	idata.suspend_hook = task_suspend_hook;
	idata.finalizer = task_finalizer;
	threadobj_init(&task->thobj, &idata);

	ret = __bt(cluster_addobj(&wind_task_table, task->name, &task->cobj));
	if (ret) {
		warning("duplicate task name: %s", task->name);
		threadobj_destroy(&task->thobj);
		__RT(pthread_mutex_destroy(&task->safelock));
		errno = S_objLib_OBJ_ID_ERROR;
		return ERROR;
	}

	registry_init_file(&task->fsobj, &registry_ops);

	ret = __bt(-__RT(pthread_create(&task->thobj.tid, &thattr,
					&task_trampoline, task)));
	pthread_attr_destroy(&thattr);
	if (ret) {
		registry_destroy_file(&task->fsobj);
		cluster_delobj(&wind_task_table, &task->cobj);
		threadobj_destroy(&task->thobj);
		__RT(pthread_mutex_destroy(&task->safelock));
		errno = ret == -EAGAIN ? S_memLib_NOT_ENOUGH_MEMORY : -ret;
		return ERROR;
	}

	return OK;
}

static inline struct wind_task *alloc_task(void)
{
	return xnmalloc(sizeof(struct wind_task));
}

STATUS taskInit(WIND_TCB *pTcb,
		const char *name,
		int prio,
		int flags,
		char *stack __attribute__ ((unused)),
		int stacksize,
		FUNCPTR entry,
		long arg0, long arg1, long arg2, long arg3, long arg4,
		long arg5, long arg6, long arg7, long arg8, long arg9)
{
	struct wind_task_args *args;
	struct wind_task *task;
	struct service svc;
	STATUS ret = ERROR;

	if (threadobj_async_p()) {
		errno = S_intLib_NOT_ISR_CALLABLE;
		return ERROR;
	}
	  
	COPPERPLATE_PROTECT(svc);

	task = alloc_task();
	if (task == NULL) {
		errno = S_memLib_NOT_ENOUGH_MEMORY;
		goto out;
	}

	args = &task->args;
	args->entry = entry;
	args->arg0 = arg0;
	args->arg1 = arg1;
	args->arg2 = arg2;
	args->arg3 = arg3;
	args->arg4 = arg4;
	args->arg5 = arg5;
	args->arg6 = arg6;
	args->arg7 = arg7;
	args->arg8 = arg8;
	args->arg9 = arg9;
	ret = __taskInit(task, pTcb, name, prio, flags, entry, stacksize);
out:
	COPPERPLATE_UNPROTECT(svc);

	return ret;
}

STATUS taskActivate(TASK_ID tid)
{
	struct wind_task *task;

	task = get_wind_task(tid);
	if (task == NULL)
		return ERROR;

	task->tcb->status &= ~WIND_SUSPEND;
	threadobj_start(&task->thobj);
	put_wind_task(task);

	return OK;
}

TASK_ID taskSpawn(const char *name,
		  int prio,
		  int flags,
		  int stacksize,
		  FUNCPTR entry,
		  long arg0, long arg1, long arg2, long arg3, long arg4,
		  long arg5, long arg6, long arg7, long arg8, long arg9)
{
	struct wind_task_args *args;
	struct wind_task *task;
	struct service svc;
	TASK_ID tid;

	if (threadobj_async_p()) {
		errno = S_intLib_NOT_ISR_CALLABLE;
		return ERROR;
	}
	  
	COPPERPLATE_PROTECT(svc);

	task = alloc_task();
	if (task == NULL) {
		errno = S_memLib_NOT_ENOUGH_MEMORY;
		COPPERPLATE_UNPROTECT(svc);
		return ERROR;
	}

	args = &task->args;
	args->entry = entry;
	args->arg0 = arg0;
	args->arg1 = arg1;
	args->arg2 = arg2;
	args->arg3 = arg3;
	args->arg4 = arg4;
	args->arg5 = arg5;
	args->arg6 = arg6;
	args->arg7 = arg7;
	args->arg8 = arg8;
	args->arg9 = arg9;

	if (__taskInit(task, &task->priv_tcb, name,
		       prio, flags, entry, stacksize) == ERROR) {
		COPPERPLATE_UNPROTECT(svc);
		return ERROR;
	}

	COPPERPLATE_UNPROTECT(svc);

	tid = mainheap_ref(&task->priv_tcb, TASK_ID);

	return taskActivate(tid) == ERROR ? ERROR : tid;
}

static STATUS __taskDelete(TASK_ID tid, int force)
{
	struct wind_task *task;
	int ret;

	if (threadobj_async_p()) {
		errno = S_intLib_NOT_ISR_CALLABLE;
		return ERROR;
	}

	task = find_wind_task_or_self(tid);
	if (task == NULL) {
	objid_error:
		errno = S_objLib_OBJ_ID_ERROR;
		return ERROR;
	}

	/*
	 * We always attempt to grab the thread safe lock first, then
	 * make sure nobody (including the target task itself) will be
	 * able to alter the internal state of that task anymore. In
	 * forced mode, we are allowed to bypass lock contention, but
	 * then we might create dangerous situations leading to
	 * invalid memory references; that's just part of the deal.
	 *
	 * NOTE: Locking order is always safelock first, internal
	 * object lock afterwards, therefore, _never_ call
	 * __taskDelete() directly or indirectly while holding the
	 * thread object lock. You have been warned.
	 *
	 * We traverse no cancellation point below until
	 * threadobj_cancel() is invoked, so we don't need any
	 * COPPERPLATE_PROTECT() section.
	 */
	if (force)	/* Best effort only. */
		force = __RT(pthread_mutex_trylock(&task->safelock));
	else
		__RT(pthread_mutex_lock(&task->safelock));

	ret = threadobj_lock(&task->thobj);

	if (!force)	/* I.e. do we own the safe lock? */
		__RT(pthread_mutex_unlock(&task->safelock));

	if (ret == 0)
		ret = threadobj_cancel(&task->thobj);

	if (ret)
		goto objid_error;

	return OK;
}

STATUS taskDelete(TASK_ID tid)
{
	return __taskDelete(tid, 0);
}

STATUS taskDeleteForce(TASK_ID tid)
{
	return __taskDelete(tid, 1);
}

TASK_ID taskIdSelf(void)
{
	struct wind_task *current;

	if (threadobj_async_p()) {
		errno = S_intLib_NOT_ISR_CALLABLE;
		return ERROR;
	}
	  
	current = wind_task_current();
	if (current == NULL) {
		errno = S_objLib_OBJ_NO_METHOD;
		return ERROR;
	}
	  
	return (TASK_ID)current->tcb;
}

struct WIND_TCB *taskTcb(TASK_ID tid)
{
	struct wind_task *task;

	task = find_wind_task(tid);
	if (task == NULL) {
		errno = S_objLib_OBJ_ID_ERROR;
		return NULL;
	}

	return task->tcb;
}

STATUS taskSuspend(TASK_ID tid)
{
	struct wind_task *task;
	struct service svc;
	int ret;

	task = get_wind_task(tid);
	if (task == NULL)
		goto objid_error;

	COPPERPLATE_PROTECT(svc);
	ret = threadobj_suspend(&task->thobj);
	COPPERPLATE_UNPROTECT(svc);
	put_wind_task(task);

	if (ret) {
	objid_error:
		errno = S_objLib_OBJ_ID_ERROR;
		return ERROR;
	}

	return OK;
}

STATUS taskResume(TASK_ID tid)
{
	struct wind_task *task;
	struct service svc;
	int ret;

	task = get_wind_task(tid);
	if (task == NULL)
		goto objid_error;

	COPPERPLATE_PROTECT(svc);
	ret = threadobj_resume(&task->thobj);
	COPPERPLATE_UNPROTECT(svc);
	put_wind_task(task);

	if (ret) {
	objid_error:
		errno = S_objLib_OBJ_ID_ERROR;
		return ERROR;
	}

	return OK;
}

STATUS taskSafe(void)
{
	struct wind_task *current;

	if (threadobj_async_p()) {
		errno = S_intLib_NOT_ISR_CALLABLE;
		return ERROR;
	}

	current = wind_task_current();
	if (current == NULL) {
		errno = S_objLib_OBJ_NO_METHOD;
		return ERROR;
	}

	/*
	 * Grabbing the safelock will lock out cancellation requests,
	 * so we don't have to issue COPPERPLATE_PROTECT().
	 */
	__RT(pthread_mutex_lock(&current->safelock));
	current->tcb->safeCnt++;

	return OK;
}

STATUS taskUnsafe(void)
{
	struct wind_task *current;
	int ret;

	if (threadobj_async_p()) {
		errno = S_intLib_NOT_ISR_CALLABLE;
		return ERROR;
	}

	current = wind_task_current();
	if (current == NULL) {
		errno = S_objLib_OBJ_NO_METHOD;
		return ERROR;
	}

	ret = __RT(pthread_mutex_unlock(&current->safelock));
	if (ret == 0)
		current->tcb->safeCnt--;

	return OK;
}

STATUS taskIdVerify(TASK_ID tid)
{
	struct wind_task *task;

	task = find_wind_task(tid);
	if (task == NULL) {
		errno = S_objLib_OBJ_ID_ERROR;
		return ERROR;
	}

	return OK;
}

void taskExit(int code)
{
	pthread_exit((void *)(long)code);
}

STATUS taskPrioritySet(TASK_ID tid, int prio)
{
	struct wind_task *task;
	struct service svc;
	int ret, cprio;

	task = get_wind_task(tid);
	if (task == NULL)
		goto objid_error;

	ret = check_task_priority(prio, &cprio);
	if (ret) {
		put_wind_task(task);
		errno = ret;
		return ERROR;
	}

	COPPERPLATE_PROTECT(svc);
	ret = threadobj_set_priority(&task->thobj, cprio);
	COPPERPLATE_UNPROTECT(svc);
	put_wind_task(task);

	if (ret) {
	objid_error:
		errno = S_objLib_OBJ_ID_ERROR;
		return ERROR;
	}

	return OK;
}

int wind_task_get_priority(struct wind_task *task)
{
	/* Can't fail if we hold the task lock as we should. */
	int prio = threadobj_get_priority(&task->thobj);
	return wind_task_denormalize_priority(prio);
}

STATUS taskPriorityGet(TASK_ID tid, int *priop)
{
	struct wind_task *task;

	task = get_wind_task(tid);
	if (task == NULL) {
		errno = S_objLib_OBJ_ID_ERROR;
		return ERROR;
	}

	*priop = wind_task_get_priority(task);
	put_wind_task(task);

	return OK;
}

STATUS taskLock(void)
{
	struct wind_task *task;
	struct service svc;

	if (threadobj_async_p()) {
		errno = S_intLib_NOT_ISR_CALLABLE;
		return ERROR;
	}

	task = get_wind_task_or_self(0);
	if (task == NULL) {
		errno = S_objLib_OBJ_ID_ERROR;
		return ERROR;
	}

	COPPERPLATE_PROTECT(svc);
	threadobj_lock_sched(&task->thobj);
	COPPERPLATE_UNPROTECT(svc);
	put_wind_task(task);

	return OK;
}

STATUS taskUnlock(void)
{
	struct wind_task *task;
	struct service svc;

	if (threadobj_async_p()) {
		errno = S_intLib_NOT_ISR_CALLABLE;
		return ERROR;
	}

	task = get_wind_task_or_self(0);
	if (task == NULL) {
		errno = S_objLib_OBJ_ID_ERROR;
		return ERROR;
	}

	COPPERPLATE_PROTECT(svc);
	threadobj_unlock_sched(&task->thobj);
	COPPERPLATE_UNPROTECT(svc);
	put_wind_task(task);

	return OK;
}

STATUS taskDelay(int ticks)
{
	struct wind_task *current;
	struct timespec rqt;
	struct service svc;
	int ret;

	if (threadobj_async_p()) {
		errno = S_intLib_NOT_ISR_CALLABLE;
		return ERROR;
	}

	current = wind_task_current();
	if (current == NULL) {
		errno = S_objLib_OBJ_NO_METHOD;
		return ERROR;
	}

	if (ticks == 0) {
		sched_yield();	/* Manual round-robin. */
		return OK;
	}

	COPPERPLATE_PROTECT(svc);

	clockobj_ticks_to_timeout(&wind_clock, ticks, &rqt);
	current->tcb->status |= WIND_DELAY;
	ret = threadobj_sleep(&rqt);
	current->tcb->status &= ~WIND_DELAY;
	if (ret) {
		errno = -ret;
		ret = ERROR;
	}

	COPPERPLATE_UNPROTECT(svc);

	return ret;
}
