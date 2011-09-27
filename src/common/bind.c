#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <nucleus/heap.h>
#include <asm/xenomai/syscall.h>
#include <asm-generic/xenomai/bits/current.h>
#include <asm-generic/xenomai/timeconv.h>
#include <asm-generic/xenomai/stack.h>
#include <asm-generic/xenomai/sem_heap.h>
#include <asm/xenomai/bits/bind.h>

int xeno_sigxcpu_no_mlock = 1;
static pthread_t xeno_main_tid;

static void xeno_sigill_handler(int sig)
{
	fprintf(stderr, "Xenomai disabled (modprobe xeno_nucleus?)\n");
	exit(EXIT_FAILURE);
}

struct xnfeatinfo xeno_featinfo;

#ifdef xeno_arch_features_check
static void do_init_arch_features(void)
{
	xeno_arch_features_check(&xeno_featinfo);
}
static void xeno_init_arch_features(void)
{
	static pthread_once_t init_archfeat_once = PTHREAD_ONCE_INIT;
	pthread_once(&init_archfeat_once, do_init_arch_features);
}
#else  /* !xeno_init_arch_features */
#define xeno_init_arch_features()	do { } while (0)
#endif /* !xeno_arch_features_check */

int xeno_bind_skin_opt(unsigned int skin_magic, const char *skin,
		       const char *module)
{
	sighandler_t old_sigill_handler;
	struct xnbindreq breq;
	struct xnfeatinfo *f;
	int muxid;

	/* Some sanity checks first. */
	if (access(XNHEAP_DEV_NAME, 0)) {
		fprintf(stderr, "Xenomai: %s is missing\n(chardev, major=10 minor=%d)\n",
			XNHEAP_DEV_NAME, XNHEAP_DEV_MINOR);
		exit(EXIT_FAILURE);
	}

	old_sigill_handler = signal(SIGILL, xeno_sigill_handler);
	if (old_sigill_handler == SIG_ERR) {
		perror("signal(SIGILL)");
		exit(EXIT_FAILURE);
	}

	f = &breq.feat_ret;
	breq.feat_req = XENOMAI_FEAT_DEP;
	breq.abi_rev = XENOMAI_ABI_REV;
	muxid = XENOMAI_SYSBIND(skin_magic, &breq);

	signal(SIGILL, old_sigill_handler);

	switch (muxid) {
	case -EINVAL:
		fprintf(stderr, "Xenomai: incompatible feature set\n");
		fprintf(stderr,
			"(userland requires \"%s\", kernel provides \"%s\", missing=\"%s\").\n",
			f->feat_man_s, f->feat_all_s, f->feat_mis_s);
		exit(EXIT_FAILURE);

	case -ENOEXEC:
		fprintf(stderr, "Xenomai: incompatible ABI revision level\n");
		fprintf(stderr, "(user-space requires '%lu', kernel provides '%lu').\n",
			XENOMAI_ABI_REV, f->feat_abirev);
		exit(EXIT_FAILURE);

	case -ENOSYS:
	case -ESRCH:
		return -1;
	}

	if (muxid < 0) {
		fprintf(stderr, "Xenomai: binding failed: %s.\n",
			strerror(-muxid));
		exit(EXIT_FAILURE);
	}

	xeno_featinfo = *f;
	xeno_init_arch_features();

	xeno_init_sem_heaps();

	xeno_init_current_keys();

	xeno_main_tid = pthread_self();

	xeno_init_timeconv(muxid);

	return muxid;
}

void xeno_fault_stack(void)
{
	if (pthread_self() == xeno_main_tid) {
		char stk[xeno_stacksize(1)];

		stk[0] = stk[sizeof(stk) - 1] = 0xA5;
	}
}

void xeno_handle_mlock_alert(int sig, siginfo_t *si, void *context)
{
	struct sigaction sa;

	if (si->si_value.sival_int == SIGDEBUG_NOMLOCK) {
		fprintf(stderr, "Xenomai: process memory not locked "
			"(missing mlockall?)\n");
		fflush(stderr);
		exit(4);
	}

	/* XNTRAPSW was set for the thread but no user-defined handler
	   has been set to override our internal handler, so let's
	   invoke the default signal action. */

	sa.sa_handler = SIG_DFL;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGXCPU, &sa, NULL);
	pthread_kill(pthread_self(), SIGXCPU);
}
