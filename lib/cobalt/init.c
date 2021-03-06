/*
 * Copyright (C) 2005 Philippe Gerum <rpm@xenomai.org>.
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
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <sys/types.h>
#include <cobalt/posix.h>
#include <cobalt/syscall.h>
#include <rtdm/syscall.h>
#include <kernel/cobalt/mutex.h>
#include <rtdk.h>
#include <asm/xenomai/bits/bind.h>

int __pse51_muxid = -1;
int __rtdm_muxid = -1;
int __rtdm_fd_start = INT_MAX;
static int fork_handler_registered;

int __wrap_pthread_setschedparam(pthread_t, int, const struct sched_param *);
void pse51_clock_init(int);

static __attribute__ ((constructor))
void __init_posix_interface(void)
{
	struct xnbindreq breq;
	
#ifndef CONFIG_XENO_LIBS_DLOPEN
	struct sched_param parm;
	int policy;
#endif /* !CONFIG_XENO_LIBS_DLOPEN */
	int muxid, err;

	rt_print_auto_init(1);

	muxid =
	    xeno_bind_skin(PSE51_SKIN_MAGIC, "POSIX", "xeno_posix");

	pse51_clock_init(muxid);

	__pse51_muxid = __xn_mux_shifted_id(muxid);

	breq.feat_req = XENOMAI_FEAT_DEP;
	breq.abi_rev = XENOMAI_ABI_REV;
	muxid = XENOMAI_SYSBIND(RTDM_SKIN_MAGIC, &breq);
	if (muxid > 0) {
		__rtdm_muxid = __xn_mux_shifted_id(muxid);
		__rtdm_fd_start = FD_SETSIZE - XENOMAI_SKINCALL0(__rtdm_muxid,
								 __rtdm_fdcount);
	}

	/* If not dlopening, we are going to shadow the main thread, so mlock
	   the whole memory for the time of the syscall, in order to avoid the
	   SIGXCPU signal. */
#if defined(CONFIG_XENO_POSIX_AUTO_MLOCKALL) || !defined(CONFIG_XENO_LIBS_DLOPEN)
	if (mlockall(MCL_CURRENT | MCL_FUTURE)) {
		perror("Xenomai Posix skin init: mlockall");
		exit(EXIT_FAILURE);
	}
#endif /* auto mlockall || !dlopen */

	/* Don't use auto-shadowing if we are likely invoked from dlopen. */
#ifndef CONFIG_XENO_LIBS_DLOPEN
	err = __STD(pthread_getschedparam(pthread_self(), &policy, &parm));
	if (err) {
		fprintf(stderr, "Xenomai Posix skin init: "
			"pthread_getschedparam: %s\n", strerror(err));
		exit(EXIT_FAILURE);
	}

	err = __wrap_pthread_setschedparam(pthread_self(), policy, &parm);
	if (err) {
		fprintf(stderr, "Xenomai Posix skin init: "
			"pthread_setschedparam: %s\n", strerror(err));
		exit(EXIT_FAILURE);
	}

#ifndef CONFIG_XENO_POSIX_AUTO_MLOCKALL
	if (munlockall()) {
		perror("Xenomai Posix skin init: munlockall");
		exit(EXIT_FAILURE);
	}
#endif /* !CONFIG_XENO_POSIX_AUTO_MLOCKALL */
#endif /* !CONFIG_XENO_LIBS_DLOPEN */

	if (fork_handler_registered)
		return;

	err = pthread_atfork(NULL, NULL, &__init_posix_interface);
	if (err) {
		fprintf(stderr, "Xenomai Posix skin init: "
			"pthread_atfork: %s\n", strerror(err));
		exit(EXIT_FAILURE);
	}
	fork_handler_registered = 1;

	if (sizeof(struct __shadow_mutex) > sizeof(pthread_mutex_t)) {
		fprintf(stderr, "sizeof(pthread_mutex_t): %d <"
			" sizeof(shadow_mutex): %d !\n",
			(int) sizeof(pthread_mutex_t),
			(int) sizeof(struct __shadow_mutex));
		exit(EXIT_FAILURE);
	}
}
