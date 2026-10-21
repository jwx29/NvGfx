/*
 * XeGfx: FreeBSD threads and scheduling on XNU: kthreads on kernel_thread_start, CPU binding through thread_bind,
 * the proc0/allproc/tid registry behind pfind and tdfind, the ck_epoch calls LinuxKPI's RCU is built on, group task
 * queues for tasklets, kernel FPU sections, Giant and the CPUID globals.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 *
 * The ck_epoch functions follow Concurrency Kit's ck_epoch.c (BSD-2-Clause, Copyright 2011-2015 Samy Al Bahra),
 * since ck_epoch_begin/end are inlines in ck_epoch.h and must see the same record and bucket behaviour.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sx.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/signalvar.h>
#include <sys/rtprio.h>
#include <sys/kthread.h>
#include <sys/unistd.h>
#include <sys/malloc.h>
#include <sys/smp.h>
#include <sys/pcpu.h>
#include <sys/_task.h>
#include <sys/gtaskqueue.h>
#include <machine/atomic.h>
#include <machine/cpu.h>
#include <machine/cpufunc.h>
#include <machine/specialreg.h>
#include <x86/cputypes.h>
#include <machine/md_var.h>
#include <machine/clock.h>
#include <machine/fpu.h>
#include <ck_epoch.h>
/* linux/sched.h (for put_task_struct) pulls in pmap.h inlines that read PCPU members the shim has no per-CPU
 * copy of; they are never called from here */
#ifndef XG_PCPU_pcid_invlpg_workaround
#define XG_PCPU_pcid_invlpg_workaround()	0
#endif
#ifndef XG_PCPU_zpcpu_offset
#define XG_PCPU_zpcpu_offset()	0
#endif
#include <linux/sched.h>
#include "xg_sched.h"
#include "xg_sched_xnu.h"

static MALLOC_DEFINE(M_XGSCHED, "xgsched", "kthreads and group task queues");

/* defined in xg_lock.c: the spin mutex every thread shadow's td_lock points to */
extern struct mtx xg_thread_lock_mtx;

/* td_flags bits FreeBSD reserves for the scheduler; td_oncpu holds the bound CPU while XG_TDF_BOUND is set */
#define XG_TDF_BOUND	TDF_SCHED0
#define XG_TDF_FPU	TDF_SCHED1
/* XNU thread_t of a kthread made by kthread_add (a reference is held until kthread_exit) */
#define XG_TD_XTHREAD(td)	((td)->td_emuldata)

/*
 * Lock classes as xg_lock.c numbers them (XG_LC_MTX 1, XG_LC_SX 3), so Giant and allproc_lock are usable before
 * any SYSINIT runs.
 */
struct mtx Giant = {
	.lock_object = { .lo_name = "Giant",
	    .lo_flags = (1 << LO_CLASSSHIFT) | LO_INITIALIZED | LO_RECURSABLE },
	.mtx_lock = MTX_UNOWNED,
};
struct sx allproc_lock = {
	.lock_object = { .lo_name = "allproc", .lo_flags = (3 << LO_CLASSSHIFT) | LO_INITIALIZED },
	.sx_lock = SX_LOCK_UNLOCKED,
};
struct proclist allproc = LIST_HEAD_INITIALIZER(allproc);

int
linux_alloc_current_noop(struct thread *td __unused, int flags __unused)
{
	return (0);
}
int (*lkpi_alloc_current)(struct thread *, int) = linux_alloc_current_noop;

/* CPUID globals (x86_var.h, clock.h); filled by xg_sched_init() */
u_int cpu_feature;
u_int cpu_feature2;
u_int cpu_stdext_feature;
u_int cpu_clflush_line_size = 32;
u_int cpu_id;
u_int cpu_vendor_id;
uint64_t tsc_freq;

/* raw spin locks with interrupts off: usable from interrupt context and without a curthread shadow */
static int
xg_sched_spin_lock(volatile u_int *l)
{
	int en = ml_set_interrupts_enabled(0);

	while (!atomic_cmpset_acq_int(l, 0, 1))
		cpu_spinwait();
	return (en);
}

static void
xg_sched_spin_unlock(volatile u_int *l, int en)
{
	atomic_store_rel_int(l, 0);
	ml_set_interrupts_enabled(en);
}

/* ---- CPUID ---- */
static void
xg_sched_identify(void)
{
	u_int r[4], max;
	char vendor[13];

	do_cpuid(0, r);
	max = r[0];
	memcpy(vendor, &r[1], 4);
	memcpy(vendor + 4, &r[3], 4);
	memcpy(vendor + 8, &r[2], 4);
	vendor[12] = 0;
	if (strcmp(vendor, "GenuineIntel") == 0)
		cpu_vendor_id = CPU_VENDOR_INTEL;
	else if (strcmp(vendor, "AuthenticAMD") == 0)
		cpu_vendor_id = CPU_VENDOR_AMD;
	else if (strcmp(vendor, "HygonGenuine") == 0)
		cpu_vendor_id = CPU_VENDOR_HYGON;
	else if (strcmp(vendor, "CentaurHauls") == 0 || strcmp(vendor, "  Shanghai  ") == 0)
		cpu_vendor_id = CPU_VENDOR_CENTAUR;
	else
		cpu_vendor_id = 0;

	do_cpuid(1, r);
	cpu_id = r[0];
	cpu_feature = r[3];
	cpu_feature2 = r[2];
	if (cpu_feature & CPUID_CLFSH)
		cpu_clflush_line_size = ((r[1] >> 8) & 0xff) * 8;
	if (max >= 7) {
		cpuid_count(7, 0, r);
		cpu_stdext_feature = r[1];
	}

	/* TSC rate: crystal * ratio (leaf 0x15), else the nominal base clock (leaf 0x16), which is what Intel's
	 * client parts run the TSC at */
	tsc_freq = 0;
	if (max >= 0x15) {
		do_cpuid(0x15, r);
		if (r[0] != 0 && r[1] != 0 && r[2] != 0)
			tsc_freq = (uint64_t)r[2] * r[1] / r[0];
	}
	if (tsc_freq == 0 && max >= 0x16) {
		do_cpuid(0x16, r);
		tsc_freq = (uint64_t)(r[0] & 0xffff) * 1000000;
	}
	if (tsc_freq == 0) {
		/* last resort: count TSC ticks over 10 ms */
		uint64_t t0, t1;
		unsigned long long a0, a1, ns;

		a0 = mach_absolute_time();
		t0 = rdtsc();
		IODelay(10000);
		t1 = rdtsc();
		a1 = mach_absolute_time();
		absolutetime_to_nanoseconds(a1 - a0, &ns);
		if (ns != 0)
			tsc_freq = (t1 - t0) * 1000000000ull / ns;
	}
}

/* ---- proc0, allproc and the thread registry ---- */
#define XG_TID_BUCKETS 256
/* LinuxKPI's list.h takes the LIST_HEAD name, so the bucket type is spelled out */
static struct { struct thread *lh_first; } xg_sched_tidhash[XG_TID_BUCKETS];
static volatile u_int xg_sched_reg_lock;
static volatile u_int xg_sched_state;		/* 0 cold, 1 initialising, 2 up */

static void
xg_sched_mtx_init(struct mtx *m, const char *name, int opts)
{
	if ((m->lock_object.lo_flags & LO_INITIALIZED) == 0)
		mtx_init(m, name, NULL, opts);
}

void
xg_sched_init(void)
{
	if (atomic_load_acq_int(&xg_sched_state) == 2)
		return;
	if (!atomic_cmpset_int(&xg_sched_state, 0, 1)) {
		while (atomic_load_acq_int(&xg_sched_state) != 2)
			cpu_spinwait();
		return;
	}
	xg_sched_identify();
	/* xg_kern.c and xg_lock.c leave these zeroed; a zeroed lock word reads as owned and would hang */
	xg_sched_mtx_init(&xg_thread_lock_mtx, "thread lock", MTX_SPIN | MTX_RECURSE);
	xg_sched_mtx_init(&proc0.p_mtx, "process lock", MTX_DEF | MTX_DUPOK);
	xg_sched_mtx_init(&proc0.p_slock, "process slock", MTX_SPIN);
	xg_sched_mtx_init(&proc0.p_statmtx, "pstatl", MTX_SPIN);
	xg_sched_mtx_init(&proc0.p_itimmtx, "pitiml", MTX_SPIN);
	xg_sched_mtx_init(&proc0.p_profmtx, "pprofl", MTX_SPIN);
	if (proc0.p_threads.tqh_last == NULL)
		TAILQ_INIT(&proc0.p_threads);
	proc0.p_state = PRS_NORMAL;
	LIST_INSERT_HEAD(&allproc, &proc0, p_list);
	atomic_store_rel_int(&xg_sched_state, 2);
}

static void
xg_sched_sysinit(void *arg __unused)
{
	xg_sched_init();
}
SYSINIT(xg_sched, SI_SUB_LOCK, SI_ORDER_FIRST, xg_sched_sysinit, NULL);

static u_int
xg_sched_tid_bucket(lwpid_t tid)
{
	return ((u_int)tid % XG_TID_BUCKETS);
}

void
xg_sched_td_register(struct thread *td)
{
	struct proc *p;
	int en;

	if (td->td_hash.le_prev != NULL)
		return;
	xg_sched_init();
	if (td->td_lock == NULL)
		td->td_lock = &xg_thread_lock_mtx;
	if (td->td_proc == NULL)
		td->td_proc = &proc0;
	p = td->td_proc;
	en = xg_sched_spin_lock(&xg_sched_reg_lock);
	if (td->td_hash.le_prev == NULL) {
		LIST_INSERT_HEAD(&xg_sched_tidhash[xg_sched_tid_bucket(td->td_tid)], td, td_hash);
		/* readers walk p_threads under PROC_LOCK only, so the new tail is published last */
		td->td_plist.tqe_next = NULL;
		td->td_plist.tqe_prev = p->p_threads.tqh_last;
		atomic_store_rel_ptr((volatile uintptr_t *)p->p_threads.tqh_last, (uintptr_t)td);
		p->p_threads.tqh_last = &td->td_plist.tqe_next;
		p->p_numthreads++;
	}
	xg_sched_spin_unlock(&xg_sched_reg_lock, en);
}

/* the caller holds PROC_LOCK(td->td_proc), so no p_threads walker is inside the list */
static void
xg_sched_td_unregister_locked(struct thread *td)
{
	struct proc *p = td->td_proc;
	int en;

	en = xg_sched_spin_lock(&xg_sched_reg_lock);
	if (td->td_hash.le_prev != NULL) {
		LIST_REMOVE(td, td_hash);
		td->td_hash.le_prev = NULL;
		TAILQ_REMOVE(&p->p_threads, td, td_plist);
		p->p_numthreads--;
	}
	xg_sched_spin_unlock(&xg_sched_reg_lock, en);
}

struct proc *
pfind(pid_t pid)
{
	xg_sched_init();
	if (pid != proc0.p_pid)
		return (NULL);
	PROC_LOCK(&proc0);
	return (&proc0);
}

struct thread *
tdfind(lwpid_t tid, pid_t pid)
{
	struct thread *td;
	struct proc *p;
	int en;

	xg_sched_init();
	xg_sched_td_register(curthread);
	en = xg_sched_spin_lock(&xg_sched_reg_lock);
	LIST_FOREACH(td, &xg_sched_tidhash[xg_sched_tid_bucket(tid)], td_hash)
		if (td->td_tid == tid)
			break;
	xg_sched_spin_unlock(&xg_sched_reg_lock, en);
	if (td == NULL)
		return (NULL);
	/* shadows are never freed; kthread_exit unregisters under PROC_LOCK, so recheck once it is held */
	p = td->td_proc;
	PROC_LOCK(p);
	if (td->td_hash.le_prev == NULL || td->td_tid != tid || (pid != -1 && p->p_pid != pid)) {
		PROC_UNLOCK(p);
		return (NULL);
	}
	return (td);
}

/* ---- signals ---- */
void
tdsignal(struct thread *td, int sig)
{
	void *th;

	if (!_SIG_VALID(sig))
		panic("tdsignal: invalid signal %d", sig);
	SIGADDSET(td->td_siglist, sig);
	/* an interruptible XNU wait (xg_lock's ABORTSAFE sleeps) ends with EINTR; others are left alone */
	th = XG_TD_XTHREAD(td);
	if (th != NULL && td != curthread)
		clear_wait(th, XG_THREAD_INTERRUPTED);
}

/* ---- priorities: recorded for FreeBSD's bookkeeping; XNU keeps its own (callers hold a spin lock) ---- */
void
sched_prio(struct thread *td, u_char prio)
{
	td->td_base_pri = prio;
	if ((td->td_flags & TDF_BORROWING) && td->td_priority < prio)
		return;
	td->td_priority = prio;
}

int
rtp_to_pri(struct rtprio *rtp, struct thread *td)
{
	u_char newpri, oldclass, oldpri;

	switch (RTP_PRIO_BASE(rtp->type)) {
	case RTP_PRIO_REALTIME:
		if (rtp->prio > RTP_PRIO_MAX)
			return (EINVAL);
		newpri = (u_char)(PRI_MIN_REALTIME + rtp->prio);
		break;
	case RTP_PRIO_NORMAL:
		if (rtp->prio > (PRI_MAX_TIMESHARE - PRI_MIN_TIMESHARE))
			return (EINVAL);
		newpri = (u_char)(PRI_MIN_TIMESHARE + rtp->prio);
		break;
	case RTP_PRIO_IDLE:
		if (rtp->prio > RTP_PRIO_MAX)
			return (EINVAL);
		newpri = (u_char)(PRI_MIN_IDLE + rtp->prio);
		break;
	default:
		return (EINVAL);
	}
	xg_sched_td_register(td);
	thread_lock(td);
	oldclass = td->td_pri_class;
	td->td_pri_class = (u_char)rtp->type;
	oldpri = td->td_user_pri;
	td->td_base_user_pri = newpri;
	if (td->td_lend_user_pri > newpri)
		td->td_user_pri = newpri;
	if (td->td_user_pri != oldpri && (oldclass != RTP_PRIO_NORMAL || td->td_pri_class != RTP_PRIO_NORMAL))
		sched_prio(td, td->td_user_pri);
	thread_unlock(td);
	return (0);
}

/* ---- CPU binding ---- */
static int
xg_sched_can_block(void)
{
	return (preemption_enabled() && !ml_at_interrupt_context());
}

/* the XNU binding that matches td's FreeBSD state: its bound CPU, the current CPU while pinned, or none */
static void
xg_sched_apply(struct thread *td)
{
	if (ml_at_interrupt_context())
		return;
	if (td->td_flags & XG_TDF_BOUND)
		thread_bind(cpu_to_processor(td->td_oncpu));
	else if (td->td_pinned > 0) {
		_disable_preemption();
		thread_bind(current_processor());
		_enable_preemption();
	} else
		thread_bind(XG_PROCESSOR_NULL);
}

void
xg_sched_pin(void)
{
	struct thread *td = curthread;

	if (td->td_pinned++ == 0 && !(td->td_flags & XG_TDF_BOUND))
		xg_sched_apply(td);
	atomic_interrupt_fence();
}

void
xg_sched_unpin(void)
{
	struct thread *td = curthread;

	atomic_interrupt_fence();
	if (--td->td_pinned == 0 && !(td->td_flags & XG_TDF_BOUND))
		xg_sched_apply(td);
}

void
sched_unbind(struct thread *td)
{
	if (td != curthread)
		panic("sched_unbind: can only unbind curthread");
	if ((td->td_flags & XG_TDF_BOUND) == 0)
		return;
	td->td_flags &= ~XG_TDF_BOUND;
	td->td_pinned--;
	xg_sched_apply(td);
}

int
sched_is_bound(struct thread *td)
{
	return ((td->td_flags & XG_TDF_BOUND) != 0);
}

void
sched_bind(struct thread *td, int cpu)
{
	int held, here;

	if (td != curthread)
		panic("sched_bind: can only bind curthread");
	if (cpu < 0 || cpu >= mp_ncpus)
		panic("sched_bind: bad cpu %d", cpu);
	if (td->td_flags & XG_TDF_BOUND)
		sched_unbind(td);
	td->td_flags |= XG_TDF_BOUND;
	td->td_oncpu = cpu;
	td->td_pinned++;
	if (ml_at_interrupt_context())
		return;
	_disable_preemption();
	here = cpu_number() == cpu;
	thread_bind(cpu_to_processor(cpu));
	_enable_preemption();
	if (here)
		return;
	/* like ULE's mi_switch(SWT_BIND): give up the thread lock, move, take it again */
	held = td->td_lock != NULL && mtx_owned(td->td_lock) && !mtx_recursed(td->td_lock);
	if (held)
		thread_unlock(td);
	if (xg_sched_can_block())
		thread_block(NULL);
	if (held)
		thread_lock(td);
}

/* ---- kthreads ---- */
struct xg_sched_start {
	struct xg_sched_start *next;
	void (*func)(void *);
	void *arg;
	struct thread *td;		/* set by the new thread */
	volatile int go;		/* set by sched_add (or kthread_add without RFSTOPPED) */
};
static struct xg_sched_start *xg_sched_starts;
static volatile u_int xg_sched_start_lock;

static void
xg_sched_kthread_start(void *arg, int wr __unused)
{
	struct xg_sched_start *r = arg, **pp;
	struct thread *td = curthread;
	void (*func)(void *);
	void *farg;
	int en;

	td->td_pflags |= TDP_KTHREAD;
	xg_sched_td_register(td);
	en = xg_sched_spin_lock(&xg_sched_start_lock);
	r->td = td;
	thread_wakeup_prim(&r->td, 0, XG_THREAD_AWAKENED);
	while (!r->go) {
		assert_wait((void *)&r->go, XG_THREAD_UNINT);
		xg_sched_spin_unlock(&xg_sched_start_lock, en);
		thread_block(NULL);
		en = xg_sched_spin_lock(&xg_sched_start_lock);
	}
	for (pp = &xg_sched_starts; *pp != NULL; pp = &(*pp)->next)
		if (*pp == r) {
			*pp = r->next;
			break;
		}
	xg_sched_spin_unlock(&xg_sched_start_lock, en);
	func = r->func;
	farg = r->arg;
	free(r, M_XGSCHED);
	func(farg);
	/* FreeBSD's fork_exit ends a kthread whose function returns */
	kthread_exit();
}

/* lets a thread that kthread_add left stopped run; any context */
static void
xg_sched_release(struct thread *td)
{
	struct xg_sched_start *r;
	int en;

	en = xg_sched_spin_lock(&xg_sched_start_lock);
	for (r = xg_sched_starts; r != NULL; r = r->next)
		if (r->td == td && !r->go) {
			r->go = 1;
			thread_wakeup_prim((void *)&r->go, 0, XG_THREAD_AWAKENED);
			break;
		}
	xg_sched_spin_unlock(&xg_sched_start_lock, en);
}

int
kthread_add(void (*func)(void *), void *arg, struct proc *p __unused, struct thread **newtdp, int flags,
    int pages __unused, const char *fmt, ...)
{
	struct xg_sched_start *r, **pp;
	struct thread *td;
	xg_thread_t th;
	va_list ap;
	int en;

	r = malloc(sizeof(*r), M_XGSCHED, M_WAITOK | M_ZERO);
	if (r == NULL)
		return (ENOMEM);
	r->func = func;
	r->arg = arg;
	en = xg_sched_spin_lock(&xg_sched_start_lock);
	r->next = xg_sched_starts;
	xg_sched_starts = r;
	xg_sched_spin_unlock(&xg_sched_start_lock, en);
	if (kernel_thread_start(xg_sched_kthread_start, r, &th) != 0) {
		en = xg_sched_spin_lock(&xg_sched_start_lock);
		for (pp = &xg_sched_starts; *pp != NULL; pp = &(*pp)->next)
			if (*pp == r) {
				*pp = r->next;
				break;
			}
		xg_sched_spin_unlock(&xg_sched_start_lock, en);
		free(r, M_XGSCHED);
		return (ENOMEM);
	}
	/* the shadow is made by the new thread itself (xg_curthread is keyed on the running XNU thread) */
	en = xg_sched_spin_lock(&xg_sched_start_lock);
	while (r->td == NULL) {
		assert_wait((void *)&r->td, XG_THREAD_UNINT);
		xg_sched_spin_unlock(&xg_sched_start_lock, en);
		thread_block(NULL);
		en = xg_sched_spin_lock(&xg_sched_start_lock);
	}
	td = r->td;
	xg_sched_spin_unlock(&xg_sched_start_lock, en);

	va_start(ap, fmt);
	vsnprintf(td->td_name, sizeof(td->td_name), fmt, ap);
	va_end(ap);
	thread_set_thread_name(th, td->td_name);
	XG_TD_XTHREAD(td) = th;
	if (newtdp != NULL)
		*newtdp = td;
	if ((flags & RFSTOPPED) == 0)
		xg_sched_release(td);
	return (0);
}

/* starts a kthread_add(RFSTOPPED) thread; drops the thread lock unless SRQ_HOLDTD, as FreeBSD's does */
void
sched_add(struct thread *td, int flags)
{
	xg_sched_release(td);
	if ((flags & SRQ_HOLDTD) == 0 && td->td_lock != NULL && mtx_owned(td->td_lock))
		thread_unlock(td);
}

void
kthread_exit(void)
{
	struct thread *td = curthread;
	struct proc *p = td->td_proc;
	struct task_struct *ts;
	xg_thread_t th;

	if ((td->td_pflags & TDP_KTHREAD) == 0)
		panic("kthread_exit: %s was not made by kthread_add", td->td_name);
	/* what FreeBSD's thread_dtor handler (linuxkpi_thread_dtor) does */
	ts = td->td_lkpi_task;
	if (ts != NULL) {
		td->td_lkpi_task = NULL;
		put_task_struct(ts);
	}
	PROC_LOCK(p);
	xg_sched_td_unregister_locked(td);
	th = XG_TD_XTHREAD(td);
	XG_TD_XTHREAD(td) = NULL;
	PROC_UNLOCK(p);
	/* the shadow outlives the XNU thread and may be handed to the next thread at this address */
	td->td_pflags &= ~TDP_KTHREAD;
	td->td_flags &= ~(XG_TDF_BOUND | XG_TDF_FPU);
	td->td_pinned = 0;
	SIGEMPTYSET(td->td_siglist);
	strlcpy(td->td_name, "xnu", sizeof(td->td_name));
	if (th != NULL)
		thread_deallocate(th);
	thread_terminate(current_thread());	/* does not return for a kernel_task thread */
	panic("kthread_exit: thread_terminate returned");
}

/* ---- ck_epoch (LinuxKPI RCU): readers mark per-CPU records with the inlines in ck_epoch.h ---- */
#define XG_CK_GRACE	3U
#define XG_CK_SENSE_MASK	(CK_EPOCH_SENSE - 1)
CK_STACK_CONTAINER(struct ck_epoch_record, record_next, xg_sched_ck_record)

void
ck_epoch_init(struct ck_epoch *global)
{
	ck_stack_init(&global->records);
	global->epoch = 1;
	global->n_free = 0;
	ck_pr_fence_store();
}

void
ck_epoch_register(struct ck_epoch *global, struct ck_epoch_record *record, void *ct)
{
	size_t i;

	record->global = global;
	record->state = 0;		/* CK_EPOCH_STATE_USED */
	record->active = 0;
	record->epoch = 0;
	record->n_dispatch = 0;
	record->n_peak = 0;
	record->n_pending = 0;
	record->ct = ct;
	memset(&record->local, 0, sizeof(record->local));
	for (i = 0; i < CK_EPOCH_LENGTH; i++)
		ck_stack_init(&record->pending[i]);
	ck_pr_fence_store();
	ck_stack_push_upmc(&global->records, &record->record_next);
}

void
_ck_epoch_addref(struct ck_epoch_record *record, struct ck_epoch_section *section)
{
	struct ck_epoch *global = record->global;
	struct ck_epoch_ref *ref;
	unsigned int epoch, i;

	epoch = ck_pr_load_uint(&global->epoch);
	i = epoch & XG_CK_SENSE_MASK;
	ref = &record->local.bucket[i];
	/* x86 is TSO: no fence needed against the other bucket */
	if (ref->count++ == 0)
		ref->epoch = epoch;
	section->bucket = i;
}

bool
_ck_epoch_delref(struct ck_epoch_record *record, struct ck_epoch_section *section)
{
	struct ck_epoch_ref *cur, *other;
	unsigned int i = section->bucket;

	cur = &record->local.bucket[i];
	cur->count--;
	if (cur->count > 0)
		return (false);
	/* the other bucket holds a newer epoch: advance the record's snapshot so writers can progress */
	other = &record->local.bucket[(i + 1) & XG_CK_SENSE_MASK];
	if (other->count > 0 && (int)(cur->epoch - other->epoch) < 0)
		ck_pr_store_uint(&record->epoch, other->epoch);
	return (true);
}

/* the next record still active in an epoch other than the one given; *af tells whether any was active */
static struct ck_epoch_record *
xg_sched_ck_scan(struct ck_epoch *global, struct ck_epoch_record *cr, unsigned int epoch, bool *af)
{
	ck_stack_entry_t *cursor;

	if (cr == NULL) {
		cursor = CK_STACK_FIRST(&global->records);
		*af = false;
	} else {
		cursor = &cr->record_next;
		*af = true;
	}
	while (cursor != NULL) {
		unsigned int state, active;

		cr = xg_sched_ck_record(cursor);
		state = ck_pr_load_uint(&cr->state);
		if (state & 1) {	/* CK_EPOCH_STATE_FREE */
			cursor = CK_STACK_NEXT(cursor);
			continue;
		}
		active = ck_pr_load_uint(&cr->active);
		*af |= active;
		if (active != 0 && ck_pr_load_uint(&cr->epoch) != epoch)
			return (cr);
		cursor = CK_STACK_NEXT(cursor);
	}
	return (NULL);
}

void
ck_epoch_synchronize_wait(struct ck_epoch *global, ck_epoch_wait_cb_t *cb, void *ct)
{
	struct ck_epoch_record *cr;
	unsigned int delta, epoch, goal, i;
	bool active;

	ck_pr_fence_memory();
	delta = epoch = ck_pr_load_uint(&global->epoch);
	goal = epoch + XG_CK_GRACE;
	for (i = 0, cr = NULL; i < XG_CK_GRACE - 1; cr = NULL, i++) {
		bool r;

		while (cr = xg_sched_ck_scan(global, cr, delta, &active), cr != NULL) {
			unsigned int e_d;

			ck_pr_stall();
			e_d = ck_pr_load_uint(&global->epoch);
			if (e_d == delta) {
				if (cb != NULL)
					cb(global, cr, ct);
				continue;
			}
			/* another writer moved the epoch: maybe our grace period already passed */
			delta = e_d;
			if ((goal > epoch) & (delta >= goal))
				goto leave;
			if (cb != NULL)
				cb(global, cr, ct);
			cr = NULL;
		}
		if (!active)
			break;
		r = ck_pr_cas_uint_value(&global->epoch, delta, delta + 1, &delta);
		ck_pr_fence_atomic_load();
		delta = delta + r;
	}
leave:
	ck_pr_fence_memory();
}

/* ---- group task queues (tasklets) ---- */
struct gtaskqueue {
	STAILQ_HEAD(, gtask) tq_queue;
	struct gtask *tq_running;
	const char *tq_name;
	volatile u_int tq_lock;
	int tq_exit;
	int tq_alive;
};

static void
xg_sched_gtq_worker(void *arg, int wr __unused)
{
	struct gtaskqueue *q = arg;
	struct gtask *t;
	int en;

	en = xg_sched_spin_lock(&q->tq_lock);
	for (;;) {
		while (STAILQ_EMPTY(&q->tq_queue) && !q->tq_exit) {
			assert_wait(&q->tq_queue, XG_THREAD_UNINT);
			xg_sched_spin_unlock(&q->tq_lock, en);
			thread_block(NULL);
			en = xg_sched_spin_lock(&q->tq_lock);
		}
		if (q->tq_exit)
			break;
		t = STAILQ_FIRST(&q->tq_queue);
		STAILQ_REMOVE_HEAD(&q->tq_queue, ta_link);
		t->ta_flags &= ~TASK_ENQUEUED;
		q->tq_running = t;
		xg_sched_spin_unlock(&q->tq_lock, en);
		t->ta_func(t->ta_context);
		en = xg_sched_spin_lock(&q->tq_lock);
		q->tq_running = NULL;
		thread_wakeup_prim(&q->tq_running, 0, XG_THREAD_AWAKENED);
	}
	q->tq_alive = 0;
	thread_wakeup_prim(&q->tq_alive, 0, XG_THREAD_AWAKENED);
	xg_sched_spin_unlock(&q->tq_lock, en);
	thread_terminate(current_thread());
}

struct gtaskqueue *
xg_sched_gtq_create(const char *name)
{
	struct gtaskqueue *q;
	xg_thread_t th;

	q = malloc(sizeof(*q), M_XGSCHED, M_WAITOK | M_ZERO);
	if (q == NULL)
		return (NULL);
	STAILQ_INIT(&q->tq_queue);
	q->tq_name = name;
	q->tq_alive = 1;
	if (kernel_thread_start(xg_sched_gtq_worker, q, &th) != 0) {
		free(q, M_XGSCHED);
		return (NULL);
	}
	thread_set_thread_name(th, name);
	thread_deallocate(th);
	return (q);
}

/* sleeps until cond(q, t) is false; the worker wakes tq_running after every task */
static void
xg_sched_gtq_wait(struct gtaskqueue *q, struct gtask *t, int (*cond)(struct gtaskqueue *, struct gtask *))
{
	int en;

	en = xg_sched_spin_lock(&q->tq_lock);
	while (cond(q, t)) {
		assert_wait(&q->tq_running, XG_THREAD_UNINT);
		xg_sched_spin_unlock(&q->tq_lock, en);
		thread_block(NULL);
		en = xg_sched_spin_lock(&q->tq_lock);
	}
	xg_sched_spin_unlock(&q->tq_lock, en);
}

static int
xg_sched_gtq_task_busy(struct gtaskqueue *q, struct gtask *t)
{
	return ((t->ta_flags & TASK_ENQUEUED) != 0 || q->tq_running == t);
}

static int
xg_sched_gtq_busy(struct gtaskqueue *q, struct gtask *t __unused)
{
	return (!STAILQ_EMPTY(&q->tq_queue) || q->tq_running != NULL);
}

int
grouptaskqueue_enqueue(struct gtaskqueue *q, struct gtask *t)
{
	int en;

	if (q == NULL)
		panic("grouptaskqueue_enqueue: task %p has no queue", t);
	en = xg_sched_spin_lock(&q->tq_lock);
	if (t->ta_flags & TASK_ENQUEUED) {
		xg_sched_spin_unlock(&q->tq_lock, en);
		return (0);
	}
	if (t->ta_flags & TASK_NOENQUEUE) {
		xg_sched_spin_unlock(&q->tq_lock, en);
		return (EAGAIN);
	}
	STAILQ_INSERT_TAIL(&q->tq_queue, t, ta_link);
	t->ta_flags |= TASK_ENQUEUED;
	thread_wakeup_prim(&q->tq_queue, 0, XG_THREAD_AWAKENED);
	xg_sched_spin_unlock(&q->tq_lock, en);
	return (0);
}

int
xg_sched_gtq_cancel(struct gtaskqueue *q, struct gtask *t)
{
	int en, error;

	en = xg_sched_spin_lock(&q->tq_lock);
	if (t->ta_flags & TASK_ENQUEUED) {
		STAILQ_REMOVE(&q->tq_queue, t, gtask, ta_link);
		t->ta_flags &= ~TASK_ENQUEUED;
		/* a drain of this task may be waiting for it */
		thread_wakeup_prim(&q->tq_running, 0, XG_THREAD_AWAKENED);
	}
	error = q->tq_running == t ? EBUSY : 0;
	xg_sched_spin_unlock(&q->tq_lock, en);
	return (error);
}

void
xg_sched_gtq_drain(struct gtaskqueue *q, struct gtask *t)
{
	xg_sched_gtq_wait(q, t, xg_sched_gtq_task_busy);
}

void
xg_sched_gtq_drain_all(struct gtaskqueue *q)
{
	xg_sched_gtq_wait(q, NULL, xg_sched_gtq_busy);
}

void
xg_sched_gtq_free(struct gtaskqueue *q)
{
	int en;

	xg_sched_gtq_drain_all(q);
	en = xg_sched_spin_lock(&q->tq_lock);
	q->tq_exit = 1;
	thread_wakeup_prim(&q->tq_queue, 0, XG_THREAD_AWAKENED);
	while (q->tq_alive) {
		assert_wait(&q->tq_alive, XG_THREAD_UNINT);
		xg_sched_spin_unlock(&q->tq_lock, en);
		thread_block(NULL);
		en = xg_sched_spin_lock(&q->tq_lock);
	}
	xg_sched_spin_unlock(&q->tq_lock, en);
	free(q, M_XGSCHED);
}

/* ---- kernel FPU sections ---- */
/*
 * XNU saves FPU state only at context switch, so a kernel SSE section must not be preempted and must leave the
 * registers as it found them (in a syscall they hold the user thread's live state). Every LinuxKPI user is
 * FPU_KERN_NOCTX; a ctx is accepted and handled the same way.
 */
static struct { char area[512]; } __aligned(64) xg_sched_fpu_save[XG_MAXCPU];
static u_long xg_sched_fpu_cr0[XG_MAXCPU];

void
fpu_kern_enter(struct thread *td, struct fpu_kern_ctx *ctx __unused, u_int flags __unused)
{
	u_long cr0;
	int cpu;

	if (td->td_flags & XG_TDF_FPU)
		panic("fpu_kern_enter: nested FPU section");
	critical_enter();
	td->td_flags |= XG_TDF_FPU;
	cpu = curcpu;
	cr0 = rcr0();
	xg_sched_fpu_cr0[cpu] = cr0;
	if (cr0 & CR0_TS)
		load_cr0(cr0 & ~CR0_TS);
	__asm __volatile("fxsave64 %0" : "=m" (xg_sched_fpu_save[cpu].area));
}

int
fpu_kern_leave(struct thread *td, struct fpu_kern_ctx *ctx __unused)
{
	int cpu;

	if ((td->td_flags & XG_TDF_FPU) == 0)
		panic("fpu_kern_leave: no FPU section");
	cpu = curcpu;
	__asm __volatile("fxrstor64 %0" : : "m" (xg_sched_fpu_save[cpu].area));
	if (xg_sched_fpu_cr0[cpu] & CR0_TS)
		load_cr0(xg_sched_fpu_cr0[cpu]);
	td->td_flags &= ~XG_TDF_FPU;
	critical_exit();
	return (0);
}
