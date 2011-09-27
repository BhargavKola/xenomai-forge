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
 */

#ifndef _COPPERPLATE_TIMEROBJ_H
#define _COPPERPLATE_TIMEROBJ_H

#include <time.h>

struct timerobj {
	void (*handler)(struct timerobj *tmobj);
	timer_t timer;
	struct itimerspec spec;
	struct pvholder link;
};

#ifdef __cplusplus
extern "C" {
#endif

int timerobj_init(struct timerobj *tmobj);

int timerobj_destroy(struct timerobj *tmobj);

int timerobj_start(struct timerobj *tmobj,
		   void (*handler)(struct timerobj *tmobj),
		   struct itimerspec *it);

int timerobj_stop(struct timerobj *tmobj);

int timerobj_pkg_init(void);

#ifdef __cplusplus
}
#endif

#endif /* _COPPERPLATE_TIMEROBJ_H */
