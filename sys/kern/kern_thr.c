/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2003, Jeffrey Roberson <jeff@freebsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "opt_posix.h"
#include "opt_thrworkq.h"
#include "opt_hwpmc_hooks.h"

#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/mman.h>
#include <sys/mutex.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/posix4.h>
#include <sys/ptrace.h>
#include <sys/racct.h>
#include <sys/resourcevar.h>
#include <sys/rtprio.h>
#include <sys/rwlock.h>
#include <sys/sched.h>
#include <sys/sysctl.h>
#include <sys/smp.h>
#include <sys/syscallsubr.h>
#include <sys/sysent.h>
#include <sys/sysproto.h>
#include <sys/signalvar.h>
#include <sys/sysctl.h>
#include <sys/thr.h>
#include <sys/rtprio.h>
#include <sys/thrworkq.h>
#include <sys/ucontext.h>
#include <sys/umtxvar.h>
#ifdef	HWPMC_HOOKS
#include <sys/pmckern.h>
#endif

#include <machine/frame.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>

#include <security/audit/audit.h>

/*
 * Default stack guard size for thread.  If set to zero then no
 * guard page.
 */
#define        THR_GUARD_DEFAULT       PAGE_SIZE

/*
 * XXX - These should most likely be sysctl parameters.
 */
static vm_size_t thr_stack_default = THR_STACK_DEFAULT;
static vm_size_t thr_stack_initial = THR_STACK_INITIAL;

static SYSCTL_NODE(_kern, OID_AUTO, threads, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "thread allocation");

int max_threads_per_proc = 1500;
SYSCTL_INT(_kern_threads, OID_AUTO, max_threads_per_proc, CTLFLAG_RW,
    &max_threads_per_proc, 0, "Limit on threads per proc");

static int max_threads_hits;
SYSCTL_INT(_kern_threads, OID_AUTO, max_threads_hits, CTLFLAG_RD,
    &max_threads_hits, 0, "kern.threads.max_threads_per_proc hit count");

#ifdef COMPAT_FREEBSD32

static inline int
suword_lwpid(void *addr, lwpid_t lwpid)
{
	int error;

	if (SV_CURPROC_FLAG(SV_LP64))
		error = suword(addr, lwpid);
	else
		error = suword32(addr, lwpid);
	return (error);
}

#else
#define suword_lwpid	suword
#endif

/*
 * System call interface.
 */

struct thr_create_initthr_args {
	ucontext_t ctx;
	int *tid;
};

static int
thr_create_initthr(struct thread *td, void *thunk)
{
	struct thr_create_initthr_args *args;

	/* Copy out the child tid. */
	args = thunk;
	if (args->tid != NULL && suword_lwpid(args->tid, td->td_tid))
		return (EFAULT);

	return (set_mcontext(td, &args->ctx.uc_mcontext));
}

int
sys_thr_create(struct thread *td, struct thr_create_args *uap)
    /* ucontext_t *ctx, int *id, int flags */
{
	struct thr_create_initthr_args args;
	int error;

	if ((error = copyin(uap->ctx, &args.ctx, sizeof(args.ctx))))
		return (error);
	args.tid = uap->id;
	return (thread_create(td, NULL, thr_create_initthr, &args));
}

int
sys_thr_new(struct thread *td, struct thr_new_args *uap)
    /* struct thr_param * */
{
	struct thr_param param;
	int error;

	if (uap->param_size < 0 || uap->param_size > sizeof(param))
		return (EINVAL);
	bzero(&param, sizeof(param));
	if ((error = copyin(uap->param, &param, uap->param_size)))
		return (error);
	return (kern_thr_new(td, &param));
}

static int
thr_new_initthr(struct thread *td, void *thunk)
{
	stack_t stack;
	struct thr_param *param;
	int error;

	/*
	 * Here we copy out tid to two places, one for child and one
	 * for parent, because pthread can create a detached thread,
	 * if parent wants to safely access child tid, it has to provide
	 * its storage, because child thread may exit quickly and
	 * memory is freed before parent thread can access it.
	 */
	param = thunk;
	if ((param->child_tid != NULL &&
	    suword_lwpid(param->child_tid, td->td_tid)) ||
	    (param->parent_tid != NULL &&
	    suword_lwpid(param->parent_tid, td->td_tid)))
		return (EFAULT);

	/* Set up our machine context. */
	stack.ss_sp = param->stack_base;
	stack.ss_size = param->stack_size;
	/* Set upcall address to user thread entry function. */
	error = cpu_set_upcall(td, param->start_func, param->arg, &stack);
	if (error != 0)
		return (error);
	/* Setup user TLS address and TLS pointer register. */
	return (cpu_set_user_tls(td, param->tls_base));
}

int
kern_thr_new(struct thread *td, struct thr_param *param)
{
	struct rtprio rtp, *rtpp;
	int error;

	rtpp = NULL;
	if (param->rtp != 0) {
		error = copyin(param->rtp, &rtp, sizeof(struct rtprio));
		if (error)
			return (error);
		rtpp = &rtp;
	}
	return (thread_create(td, rtpp, thr_new_initthr, param));
}

int
thread_create(struct thread *td, struct rtprio *rtp,
    int (*initialize_thread)(struct thread *, void *), void *thunk)
{
	struct thread *newtd;
	struct proc *p;
	int error;

	p = td->td_proc;

	if (rtp != NULL) {
		switch(rtp->type) {
		case RTP_PRIO_REALTIME:
		case RTP_PRIO_FIFO:
			/* Only root can set scheduler policy */
			if (priv_check(td, PRIV_SCHED_SETPOLICY) != 0)
				return (EPERM);
			if (rtp->prio > RTP_PRIO_MAX)
				return (EINVAL);
			break;
		case RTP_PRIO_NORMAL:
			rtp->prio = 0;
			break;
		default:
			return (EINVAL);
		}
	}

#ifdef RACCT
	if (racct_enable) {
		PROC_LOCK(p);
		error = racct_add(p, RACCT_NTHR, 1);
		PROC_UNLOCK(p);
		if (error != 0)
			return (EPROCLIM);
	}
#endif

	/* Initialize our td */
	error = kern_thr_alloc(p, 0, &newtd);
	if (error)
		goto fail;

	bzero(&newtd->td_startzero,
	    __rangeof(struct thread, td_startzero, td_endzero));
	bcopy(&td->td_startcopy, &newtd->td_startcopy,
	    __rangeof(struct thread, td_startcopy, td_endcopy));
	newtd->td_proc = td->td_proc;
	newtd->td_rb_list = newtd->td_rbp_list = newtd->td_rb_inact = 0;
	thread_cow_get(newtd, td);

	cpu_copy_thread(newtd, td);

	error = initialize_thread(newtd, thunk);
	if (error != 0) {
		thread_cow_free(newtd);
		thread_free(newtd);
		goto fail;
	}

	PROC_LOCK(p);
	p->p_flag |= P_HADTHREADS;
	thread_link(newtd, p);
	bcopy(p->p_comm, newtd->td_name, sizeof(newtd->td_name));
	thread_lock(td);
	/* let the scheduler know about these things. */
	sched_fork_thread(td, newtd);
	thread_unlock(td);
	if (P_SHOULDSTOP(p))
		ast_sched(newtd, TDA_SUSPEND);
	if (p->p_ptevents & PTRACE_LWP)
		newtd->td_dbgflags |= TDB_BORN;

	PROC_UNLOCK(p);
#ifdef	HWPMC_HOOKS
	if (PMC_PROC_IS_USING_PMCS(p))
		PMC_CALL_HOOK(newtd, PMC_FN_THR_CREATE, NULL);
	else if (PMC_SYSTEM_SAMPLING_ACTIVE())
		PMC_CALL_HOOK_UNLOCKED(newtd, PMC_FN_THR_CREATE_LOG, NULL);
#endif

	tidhash_add(newtd);

	/* ignore timesharing class */
	if (rtp != NULL && !(td->td_pri_class == PRI_TIMESHARE &&
	    rtp->type == RTP_PRIO_NORMAL))
		rtp_to_pri(rtp, newtd);

	thread_lock(newtd);
	TD_SET_CAN_RUN(newtd);
	sched_add(newtd, SRQ_BORING);

	return (0);

fail:
#ifdef RACCT
	if (racct_enable) {
		PROC_LOCK(p);
		racct_sub(p, RACCT_NTHR, 1);
		PROC_UNLOCK(p);
	}
#endif
	return (error);
}

int
sys_thr_self(struct thread *td, struct thr_self_args *uap)
    /* long *id */
{
	int error;

	error = suword_lwpid(uap->id, (unsigned)td->td_tid);
	if (error == -1)
		return (EFAULT);
	return (0);
}

int
sys_thr_exit(struct thread *td, struct thr_exit_args *uap)
    /* long *state */
{

#ifdef THRWORKQ
	if (td->td_reuse_stack != NULL) {
		thrworkq_reusestack(td->td_proc, td->td_reuse_stack);
		td->td_reuse_stack = NULL;
	}
#endif

	umtx_thread_exit(td);

	/* Signal userland that it can free the stack. */
	if ((void *)uap->state != NULL) {
		(void)suword_lwpid(uap->state, 1);
		(void)kern_umtx_wake(td, uap->state, INT_MAX, 0);
	}

	return (kern_thr_exit(td));
}

int
kern_thr_exit(struct thread *td)
{
	struct proc *p;

	p = td->td_proc;

	/*
	 * If all of the threads in a process call this routine to
	 * exit (e.g. all threads call pthread_exit()), exactly one
	 * thread should return to the caller to terminate the process
	 * instead of the thread.
	 *
	 * Checking p_numthreads alone is not sufficient since threads
	 * might be committed to terminating while the PROC_LOCK is
	 * dropped in either ptracestop() or while removing this thread
	 * from the tidhash.  Instead, the p_pendingexits field holds
	 * the count of threads in either of those states and a thread
	 * is considered the "last" thread if all of the other threads
	 * in a process are already terminating.
	 */
	PROC_LOCK(p);
	if (p->p_numthreads == p->p_pendingexits + 1) {
		/*
		 * Ignore attempts to shut down last thread in the
		 * proc.  This will actually call _exit(2) in the
		 * usermode trampoline when it returns.
		 */
		PROC_UNLOCK(p);
		return (0);
	}

	if (p->p_sysent->sv_ontdexit != NULL)
		p->p_sysent->sv_ontdexit(td);

	td->td_dbgflags |= TDB_EXIT;
	if (p->p_ptevents & PTRACE_LWP) {
		p->p_pendingexits++;
		ptracestop(td, SIGTRAP, NULL);
		p->p_pendingexits--;
	}
	tidhash_remove(td);

	/*
	 * The check above should prevent all other threads from this
	 * process from exiting while the PROC_LOCK is dropped, so
	 * there must be at least one other thread other than the
	 * current thread.
	 */
	KASSERT(p->p_numthreads > 1, ("too few threads"));
	racct_sub(p, RACCT_NTHR, 1);
	tdsigcleanup(td);

#ifdef AUDIT
	AUDIT_SYSCALL_EXIT(0, td);
#endif

	PROC_SLOCK(p);
	thread_stopped(p);
	thread_exit();
	/* NOTREACHED */
}

int
sys_thr_kill(struct thread *td, struct thr_kill_args *uap)
    /* long id, int sig */
{
	ksiginfo_t ksi;
	struct thread *ttd;
	struct proc *p;
	int error;

	p = td->td_proc;
	ksiginfo_init(&ksi);
	ksi.ksi_signo = uap->sig;
	ksi.ksi_code = SI_LWP;
	ksi.ksi_pid = p->p_pid;
	ksi.ksi_uid = td->td_ucred->cr_ruid;
	if (uap->id == -1) {
		if (uap->sig != 0 && !_SIG_VALID(uap->sig)) {
			error = EINVAL;
		} else {
			error = ESRCH;
			PROC_LOCK(p);
			FOREACH_THREAD_IN_PROC(p, ttd) {
				if (ttd != td) {
					error = 0;
					if (uap->sig == 0)
						break;
					tdksignal(ttd, uap->sig, &ksi);
				}
			}
			PROC_UNLOCK(p);
		}
	} else {
		error = 0;
		ttd = tdfind((lwpid_t)uap->id, p->p_pid);
		if (ttd == NULL)
			return (ESRCH);
		if (uap->sig == 0)
			;
		else if (!_SIG_VALID(uap->sig))
			error = EINVAL;
		else 
			tdksignal(ttd, uap->sig, &ksi);
		PROC_UNLOCK(ttd->td_proc);
	}
	return (error);
}

int
sys_thr_kill2(struct thread *td, struct thr_kill2_args *uap)
    /* pid_t pid, long id, int sig */
{
	ksiginfo_t ksi;
	struct thread *ttd;
	struct proc *p;
	int error;

	AUDIT_ARG_SIGNUM(uap->sig);

	ksiginfo_init(&ksi);
	ksi.ksi_signo = uap->sig;
	ksi.ksi_code = SI_LWP;
	ksi.ksi_pid = td->td_proc->p_pid;
	ksi.ksi_uid = td->td_ucred->cr_ruid;
	if (uap->id == -1) {
		if ((p = pfind(uap->pid)) == NULL)
			return (ESRCH);
		AUDIT_ARG_PROCESS(p);
		error = p_cansignal(td, p, uap->sig);
		if (error) {
			PROC_UNLOCK(p);
			return (error);
		}
		if (uap->sig != 0 && !_SIG_VALID(uap->sig)) {
			error = EINVAL;
		} else {
			error = ESRCH;
			FOREACH_THREAD_IN_PROC(p, ttd) {
				if (ttd != td) {
					error = 0;
					if (uap->sig == 0)
						break;
					tdksignal(ttd, uap->sig, &ksi);
				}
			}
		}
		PROC_UNLOCK(p);
	} else {
		ttd = tdfind((lwpid_t)uap->id, uap->pid);
		if (ttd == NULL)
			return (ESRCH);
		p = ttd->td_proc;
		AUDIT_ARG_PROCESS(p);
		error = p_cansignal(td, p, uap->sig);
		if (uap->sig == 0)
			;
		else if (!_SIG_VALID(uap->sig))
			error = EINVAL;
		else
			tdksignal(ttd, uap->sig, &ksi);
		PROC_UNLOCK(p);
	}
	return (error);
}

int
sys_thr_suspend(struct thread *td, struct thr_suspend_args *uap)
	/* const struct timespec *timeout */
{
	struct timespec ts, *tsp;
	int error;

	tsp = NULL;
	if (uap->timeout != NULL) {
		error = umtx_copyin_timeout(uap->timeout, &ts);
		if (error != 0)
			return (error);
		tsp = &ts;
	}

	return (kern_thr_suspend(td, tsp));
}

int
kern_thr_suspend(struct thread *td, struct timespec *tsp)
{
	struct proc *p = td->td_proc;
	struct timeval tv;
	int error = 0;
	int timo = 0;

	if (td->td_pflags & TDP_WAKEUP) {
		td->td_pflags &= ~TDP_WAKEUP;
		return (0);
	}

	if (tsp != NULL) {
		if (tsp->tv_sec == 0 && tsp->tv_nsec == 0)
			error = EWOULDBLOCK;
		else {
			TIMESPEC_TO_TIMEVAL(&tv, tsp);
			timo = tvtohz(&tv);
		}
	}

	PROC_LOCK(p);
	if (error == 0 && (td->td_flags & TDF_THRWAKEUP) == 0)
		error = msleep((void *)td, &p->p_mtx,
			 PCATCH, "lthr", timo);

	if (td->td_flags & TDF_THRWAKEUP) {
		thread_lock(td);
		td->td_flags &= ~TDF_THRWAKEUP;
		thread_unlock(td);
		PROC_UNLOCK(p);
		return (0);
	}
	PROC_UNLOCK(p);
	if (error == EWOULDBLOCK)
		error = ETIMEDOUT;
	else if (error == ERESTART) {
		if (timo != 0)
			error = EINTR;
	}
	return (error);
}

int
sys_thr_wake(struct thread *td, struct thr_wake_args *uap)
	/* long id */
{
	struct proc *p;
	struct thread *ttd;

	if (uap->id == td->td_tid) {
		td->td_pflags |= TDP_WAKEUP;
		return (0);
	} 

	p = td->td_proc;
	ttd = tdfind((lwpid_t)uap->id, p->p_pid);
	if (ttd == NULL)
		return (ESRCH);
	thread_lock(ttd);
	ttd->td_flags |= TDF_THRWAKEUP;
	thread_unlock(ttd);
	wakeup((void *)ttd);
	PROC_UNLOCK(p);
	return (0);
}

int
sys_thr_set_name(struct thread *td, struct thr_set_name_args *uap)
{
	struct proc *p;
	char name[MAXCOMLEN + 1];
	struct thread *ttd;
	int error;

	error = 0;
	name[0] = '\0';
	if (uap->name != NULL) {
		error = copyinstr(uap->name, name, sizeof(name), NULL);
		if (error == ENAMETOOLONG) {
			error = copyin(uap->name, name, sizeof(name) - 1);
			name[sizeof(name) - 1] = '\0';
		}
		if (error)
			return (error);
	}
	p = td->td_proc;
	ttd = tdfind((lwpid_t)uap->id, p->p_pid);
	if (ttd == NULL)
		return (ESRCH);
	strcpy(ttd->td_name, name);
#ifdef HWPMC_HOOKS
	if (PMC_PROC_IS_USING_PMCS(p) || PMC_SYSTEM_SAMPLING_ACTIVE())
		PMC_CALL_HOOK_UNLOCKED(ttd, PMC_FN_THR_CREATE_LOG, NULL);
#endif
#ifdef KTR
	sched_clear_tdname(ttd);
#endif
	PROC_UNLOCK(p);
	return (error);
}

int
sys_thr_stack(struct thread *td, struct thr_stack_args *uap)
{
	vm_size_t stacksz, guardsz;
	void *addr;
	int error;

	/* Round up to the nearest page size. */
	stacksz = (vm_size_t)round_page(uap->stacksize);
	guardsz = (vm_size_t)round_page(uap->guardsize);

	if (stacksz == 0)
		stacksz = thr_stack_default;

	error = kern_thr_stack(td->td_proc, &addr, stacksz, guardsz);

	td->td_retval[0] = (register_t) addr;

	return (error);
}

/*
 * kern_thr_stack() maps a new thread stack in the process.  It returns
 * the stack address in the 'addr' arg.
 *
 * Base address of the last stack allocated (including its red zone, if
 * there is one).  Stacks are allocated contiguously, starting beyond the
 * top of the main stack.  When a new stack is created, a red zone is
 * typically created (actually, the red zone is mapped with PROT_NONE) above
 * the top of the stack, such that the stack will not be able to grow all
 * the way to the bottom of the next stack.  This isn't fool-proof.  It is
 * possible for a stack to grow by a large amount, such that it grows into
 * the next stack, and as long as the memory within the red zone is never
 * accessed, nothing will prevent one thread stack from trouncing all over
 * the next.
 *
 * low memory
 *     . . . . . . . . . . . . . . . . . .
 *    |                                   |
 *    |             stack 3               | start of 3rd thread stack
 *    +-----------------------------------+
 *    |                                   |
 *    |       Red Zone (guard page)       | red zone for 2nd thread
 *    |                                   |
 *    +-----------------------------------+
 *    |  stack 2 - thr_stack_default      | top of 2nd thread stack
 *    |                                   |
 *    |                                   |
 *    |                                   |
 *    |                                   |
 *    |             stack 2               |
 *    +-----------------------------------+ <-- start of 2nd thread stack
 *    |                                   |
 *    |       Red Zone (guard page)       | red zone for 1st thread
 *    |                                   |
 *    +-----------------------------------+
 *    |  stack 1 - thr_stack_default      | top of 1st thread stack
 *    |                                   |
 *    |                                   |
 *    |                                   |
 *    |                                   |
 *    |             stack 1               |
 *    +-----------------------------------+ <-- start of 1st thread stack
 *    |                                   |   (initial value of p->p_thrstack)
 *    |       Red Zone (guard page)       |
 *    |                                   | red zone for main thread
 *    +-----------------------------------+
 *    | ->sv_usrstack - thr_stack_initial | top of main thread stack
 *    |                                   | ^
 *    |                                   | |
 *    |                                   | |
 *    |                                   | | stack growth
 *    |                                   |
 *    +-----------------------------------+ <-- start of main thread stack
 *                                              (p->p_sysent->sv_usrstack)
 * high memory
 *
 * XXX - This code assumes that the stack always grows down in address space.
 */
int
kern_thr_stack(struct proc *p, void **addr, vm_size_t stacksz,
    vm_size_t guardsz)
{
	vm_offset_t stackaddr;
	vm_map_t map;
	int error;

	KASSERT(stacksz != 0, ("[%s: %d] stacksz = 0", __FILE__, __LINE__));

	*addr = NULL;

	PROC_LOCK(p);
	if (p->p_thrstack == 0)  {
		/* Compute the start of the first thread stack. */
		p->p_thrstack = p->p_sysent->sv_usrstack -
		    (vm_offset_t)(thr_stack_initial + THR_GUARD_DEFAULT);
	}

	stackaddr = p->p_thrstack - (vm_offset_t)(stacksz + guardsz);

	/*
	 * Compute the next stack location unconditionally.  Under normal
	 * operating conditions, the most likely reason for no being able
	 * to map the thread stack is a stack overflow of the adjacent
	 * thread stack.
	 */
	p->p_thrstack -= (vm_offset_t)(stacksz + guardsz);
	PROC_UNLOCK(p);

	map = &p->p_vmspace->vm_map;
	error = vm_mmap(map, &stackaddr, (stacksz + guardsz), VM_PROT_ALL,
	    PROT_READ | PROT_WRITE, MAP_STACK, OBJT_DEFAULT, NULL, 0);
	if (error)
		return (error);

	if (guardsz != 0) {
		error = vm_map_protect(map, stackaddr, stackaddr + guardsz,
		    PROT_NONE, 0, 1);
		if (error) {
			/* unmap memory */
			(void) vm_map_remove(map, stackaddr, stackaddr +
			    (stacksz + guardsz));

			return (error);
		}
	}

	*addr = (void *)(stackaddr + guardsz);
	return (0);
}

int
kern_thr_alloc(struct proc *p, int pages, struct thread **ntd)
{

	/* Have race condition but it is cheap. */
	if (p->p_numthreads >= max_threads_per_proc) {
		++max_threads_hits;
		return (EPROCLIM);
	}

	*ntd = thread_alloc(pages);
	if (*ntd == NULL)
		return (ENOMEM);

	return (0);
}