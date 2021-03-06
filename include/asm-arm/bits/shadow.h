/*
 * Copyright (C) 2001,2002,2003,2004 Philippe Gerum <rpm@xenomai.org>.
 *
 * ARM port
 *   Copyright (C) 2005 Stelian Pop
 *
 * Xenomai is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Xenomai is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Xenomai; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#ifndef _XENO_ASM_ARM_BITS_SHADOW_H
#define _XENO_ASM_ARM_BITS_SHADOW_H

#ifndef __KERNEL__
#error "Pure kernel header included from user-space!"
#endif

#include <asm/cacheflush.h>

static inline void xnarch_init_shadow_tcb(xnarchtcb_t * tcb,
					  struct xnthread *thread,
					  const char *name)
{
	struct task_struct *task = current;

	tcb->user_task = task;
	tcb->active_task = NULL;
	tcb->mm = task->mm;
	tcb->active_mm = NULL;
	tcb->tip = task_thread_info(task);
#ifdef CONFIG_XENO_HW_FPU
	tcb->user_fpu_owner = task;
	tcb->fpup = (rthal_fpenv_t *) & task_thread_info(task)->used_cp[0];
#endif /* CONFIG_XENO_HW_FPU */
	tcb->entry = NULL;
	tcb->cookie = NULL;
	tcb->self = thread;
	tcb->imask = 0;
	tcb->name = name;
}

int xnarch_local_syscall(unsigned long a1, unsigned long a2,
			 unsigned long a3, unsigned long a4,
			 unsigned long a5)
{
	int error = 0;

	switch (a1) {
	case XENOMAI_SYSARCH_ATOMIC_ADD_RETURN:{
			int i;
			atomic_t *v, val;
			int ret;
			unsigned long flags;

			local_irq_save_hw(flags);
			__xn_get_user(i, (int *)a2);
			__xn_get_user(v, (atomic_t **)a3);
			if (__xn_copy_from_user(&val, v, sizeof(atomic_t))) {
				error = -EFAULT;
				goto unlock;
			}
			ret = atomic_add_return(i, &val);
			if (__xn_copy_to_user(v, &val, sizeof(atomic_t))) {
				error = -EFAULT;
				goto unlock;
			}
			__xn_put_user(ret, (int *)a4);
		  unlock:
			local_irq_restore_hw(flags);
			break;
		}
	case XENOMAI_SYSARCH_ATOMIC_SET_MASK:{
			unsigned long mask;
			unsigned long *addr, val;
			unsigned long flags;

			local_irq_save_hw(flags);
			__xn_get_user(mask, (unsigned long *)a2);
			__xn_get_user(addr, (unsigned long **)a3);
			__xn_get_user(val, (unsigned long *)addr);
			val |= mask;
			__xn_put_user(val, (unsigned long *)addr);
			local_irq_restore_hw(flags);
			break;
		}
	case XENOMAI_SYSARCH_ATOMIC_CLEAR_MASK:{
			unsigned long mask;
			unsigned long *addr, val;
			unsigned long flags;

			local_irq_save_hw(flags);
			__xn_get_user(mask, (unsigned long *)a2);
			__xn_get_user(addr, (unsigned long **)a3);
			__xn_get_user(val, (unsigned long *)addr);
			val &= ~mask;
			__xn_put_user(val, (unsigned long *)addr);
			local_irq_restore_hw(flags);
			break;
		}
	case XENOMAI_SYSARCH_XCHG:{
			void *ptr;
			unsigned long x;
			unsigned int size;
			unsigned long ret = 0;
			unsigned long flags;

			local_irq_save_hw(flags);
			__xn_get_user(ptr, (unsigned char **)a2);
			__xn_get_user(x, (unsigned long *)a3);
			__xn_get_user(size, (unsigned int *)a4);
			if (size == 4) {
				unsigned long val;
				__xn_get_user(val, (unsigned long *)ptr);
				ret = xnarch_atomic_xchg(&val, x);
			} else
				error = -EINVAL;
			__xn_put_user(ret, (unsigned long *)a5);
			local_irq_restore_hw(flags);
			break;
		}
/*
 * If I-pipe supports user-space tsc emulation, add a syscall for
 * retrieving tsc infos.
 */
#ifdef IPIPE_TSC_TYPE_NONE
	case XENOMAI_SYSARCH_TSCINFO:{
		struct ipipe_sysinfo ipipe_info;
		struct __xn_tscinfo info;

		error = ipipe_get_sysinfo(&ipipe_info);
		if (error)
			return error;

		switch (RTHAL_TSC_INFO(&ipipe_info).type) {
		case IPIPE_TSC_TYPE_FREERUNNING:
			info.type = __XN_TSC_TYPE_FREERUNNING,
			info.counter = RTHAL_TSC_INFO(&ipipe_info).u.fr.counter;
			info.mask = RTHAL_TSC_INFO(&ipipe_info).u.fr.mask;
			info.tsc = RTHAL_TSC_INFO(&ipipe_info).u.fr.tsc;
			break;
		case IPIPE_TSC_TYPE_DECREMENTER:
			info.type = __XN_TSC_TYPE_DECREMENTER,
			info.counter = RTHAL_TSC_INFO(&ipipe_info).u.dec.counter;
			info.mask = RTHAL_TSC_INFO(&ipipe_info).u.dec.mask;
			info.last_cnt = RTHAL_TSC_INFO(&ipipe_info).u.dec.last_cnt;
			info.tsc = RTHAL_TSC_INFO(&ipipe_info).u.dec.tsc;
			break;
#ifdef IPIPE_TSC_TYPE_FREERUNNING_COUNTDOWN
		case IPIPE_TSC_TYPE_FREERUNNING_COUNTDOWN:
			info.type = __XN_TSC_TYPE_FREERUNNING_COUNTDOWN,
			info.counter = RTHAL_TSC_INFO(&ipipe_info).u.fr.counter;
			info.mask = RTHAL_TSC_INFO(&ipipe_info).u.fr.mask;
			info.tsc = RTHAL_TSC_INFO(&ipipe_info).u.fr.tsc;
			break;
#endif /* IPIPE_TSC_TYPE_FREERUNNING_COUNTDOWN */
		case IPIPE_TSC_TYPE_NONE:
			return -ENOSYS;

		default:
			return -EINVAL;
		}
		
		if (__xn_copy_to_user((void *)a2, &info, sizeof(info)))
			return -EFAULT;
		break;
	}
#endif /* IPIPE_TSC_TYPE_NONE */

	default:
		error = -EINVAL;
	}
	return error;
}

#define xnarch_schedule_tail(prev) do { } while(0)

static inline void xnarch_setup_mayday_page(void *page)
{
	/*
	 * We want this code to appear at the top of the MAYDAY page:
	 *
	 * ifdef ARM_EABI
	 *
	 * e3a00f8a 	mov	r0, #552	; 0x228
	 * e28003c3 	add	r0, r0, #201326595	; 0xc000003
	 * e3a0780f 	mov	r7, #983040	; 0xf0000
	 * e2877042 	add	r7, r7, #66	; 0x42
	 * ef000000 	svc	0x00000000
	 * e3a00000 	mov	r0, #0
	 * e5800000 	str	r0, [r0]	; <bug>
	 *
	 * elif ARM_OABI
	 *
	 * e3a00f8a 	mov	r0, #552	; 0x228
	 * e28003c3 	add	r0, r0, #201326595	; 0xc000003
	 * ef9f0042 	swi	0x009f0042
	 * e3a00000 	mov	r0, #0
	 * e5800000 	str	r0, [r0]	; <bug>
	 *
	 * endif
	 *
	 * 32bit instruction words will be laid out by the compiler as
	 * the target endianness requires.
	 *
	 * We don't mess with CPSR here, so no need to save/restore it
	 * in handle/fixup code.
	 */
#ifdef CONFIG_XENO_ARM_EABI
	static const struct {
		u32 mov_muxl;
		u32 add_muxh;
		u32 mov_sysh;
		u32 add_sysl;
		u32 swi_0;
		u32 mov_r0;
		u32 str_r0;
	} code = {
		.mov_muxl = 0xe3a00f8a,
		.add_muxh = 0xe28003c3,
		.mov_sysh = 0xe3a0780f,
		.add_sysl = 0xe2877042,
		.swi_0 = 0xef000000,
		.mov_r0 = 0xe3a00000,
		.str_r0 = 0xe5800000
	};
#else /* OABI */
	static const struct {
		u32 mov_muxl;
		u32 add_muxh;
		u32 swi_syscall;
		u32 mov_r0;
		u32 str_r0;
	} code = {
		.mov_muxl = 0xe3a00f8a,
		.add_muxh = 0xe28003c3,
		.swi_syscall = 0x009f0042,
		.mov_r0 = 0xe3a00000,
		.str_r0 = 0xe5800000
	};
#endif /* OABI */

	memcpy(page, &code, sizeof(code));

	flush_dcache_page(vmalloc_to_page(page));
}

static inline void xnarch_call_mayday(struct task_struct *p)
{
	rthal_return_intercept(p);
}

static inline void xnarch_handle_mayday(struct xnarchtcb *tcb,
					struct pt_regs *regs,
					unsigned long tramp)
{
	tcb->mayday.pc = regs->ARM_pc;
	tcb->mayday.r0 = regs->ARM_r0;
#ifdef CONFIG_XENO_ARM_EABI
	tcb->mayday.r7 = regs->ARM_r7;
#endif
	regs->ARM_pc = tramp;
}

static inline void xnarch_fixup_mayday(struct xnarchtcb *tcb,
				       struct pt_regs *regs)
{
	regs->ARM_pc = tcb->mayday.pc;
	regs->ARM_r0 = tcb->mayday.r0;
#ifdef CONFIG_XENO_ARM_EABI
	regs->ARM_r7 = tcb->mayday.r7;
#endif
}

#endif /* !_XENO_ASM_ARM_BITS_SHADOW_H */
