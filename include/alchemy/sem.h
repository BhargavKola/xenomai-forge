/*
 * Copyright (C) 2011 Philippe Gerum <rpm@xenomai.org>.
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

#ifndef _XENOMAI_ALCHEMY_SEM_H
#define _XENOMAI_ALCHEMY_SEM_H

#include <stdint.h>
#include <alchemy/timer.h>

/* Creation flags. */
#define S_PRIO  0x1	/* Pend by task priority order. */
#define S_FIFO  0x0	/* Pend by FIFO order. */
#define S_PULSE 0x2	/* Enable pulse mode. */

struct RT_SEM {
	uintptr_t handle;
};

typedef struct RT_SEM RT_SEM;

struct RT_SEM_INFO {
	unsigned long count;
	int nwaiters;
	char name[32];
};

typedef struct RT_SEM_INFO RT_SEM_INFO;

#ifdef __cplusplus
extern "C" {
#endif

int rt_sem_create(RT_SEM *sem,
		  const char *name,
		  unsigned long icount,
		  int mode);

int rt_sem_delete(RT_SEM *sem);

int rt_sem_p(RT_SEM *sem,
	     RTIME timeout);

int rt_sem_p_until(RT_SEM *sem,
		   RTIME timeout);

int rt_sem_v(RT_SEM *sem);

int rt_sem_broadcast(RT_SEM *sem);

int rt_sem_inquire(RT_SEM *sem,
		   RT_SEM_INFO *info);

#ifdef __cplusplus
}
#endif

#endif /* _XENOMAI_ALCHEMY_SEM_H */
