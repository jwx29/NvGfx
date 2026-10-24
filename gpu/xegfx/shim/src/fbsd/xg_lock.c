/*
 * XeGfx: FreeBSD locks and sleep queues on XNU. The lock words keep FreeBSD's encoding (owner = curthread, flag
 * bits as in mutex.h/sx.h/rwlock.h), so the inline macros (mtx_owned, sx_xlocked, ...) stay right. Waiters sleep
 * with assert_wait() on the lock word's address, asserted under a hashed spin lock that the waker also takes, so no
 * wakeup is lost. An unlock with waiters wakes all of them and they race again: simple, no handoff.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sx.h>
#include <sys/rwlock.h>
#include <sys/condvar.h>
#include <sys/sleepqueue.h>
#include <sys/proc.h>
#include <sys/time.h>
#include <machine/atomic.h>
#include <machine/cpufunc.h>
#include "xg_xnu.h"

sbintime_t xg_sbinuptime(void);

/* the lock functions get the lock word; these recover the lock (FreeBSD defines them in kern_mutex.c/kern_rwlock.c) */
#define mtxlock2mtx(c)	(__containerof(c, struct mtx, mtx_lock))
#define rwlock2rw(c)	(__containerof(c, struct rwlock, rw_lock))

/* lock classes, kept in the lo_flags class bits */
#define XG_LC_MTX	(1 << LO_CLASSSHIFT)
#define XG_LC_SPIN	(2 << LO_CLASSSHIFT)
#define XG_LC_SX	(3 << LO_CLASSSHIFT)
#define XG_LC_RW	(4 << LO_CLASSSHIFT)
#define XG_SPIN_TRIES	256

/*
 * Every spin below should last microseconds. Before 0.0.31 they had no bound and N-258 / N-260 ended in a CPU
 * rendezvous panic and a silent freeze: a spinner with interrupts off stops answering IPIs. xg_spin_note samples
 * the clock every 64 turns and panics past 150 ms with interrupts off (under the rendezvous timeout) or 2 s with
 * them on. xegfx.spin_panic=0 in debug.xegfx_env only counts.
 */
int xg_spin_panic = 1;
unsigned long long xg_spin_max_us;	/* the longest spin seen */
unsigned int xg_spin_long;		/* spins over 10 ms */

void
xg_spin_note(struct xg_spin *s, const char *kind, const char *name, const char *file, int line, unsigned long owner)
{
	unsigned long long now, ns, us;

	if ((s->n++ & 63) != 0)
		return;
	now = mach_absolute_time();
	if (s->t0 == 0) {
		s->t0 = now;
		return;
	}
	absolutetime_to_nanoseconds(now - s->t0, &ns);
	us = ns / 1000;
	if (us > xg_spin_max_us)
		xg_spin_max_us = us;
	if (us >= 10000 && !s->counted) {
		s->counted = 1;
		xg_spin_long++;
	}
	if (xg_spin_panic && us > (ml_get_interrupts_enabled() ? 2000000ull : 150000ull))
		panic("xegfx: %s %s spun %llu ms at %s:%d, owner %#lx, critnest %d, spinlock count %d, cpu %d",
		    kind, name ? name : "?", us / 1000, file ? file : "?", line, owner, curthread->td_critnest,
		    curthread->td_md.md_spinlock_count, cpu_number());
}

/* wait-channel chains: a spin lock per bucket, held inside a critical section */
#define XG_SQ_BUCKETS 512
static struct { volatile u_int lock; char pad[CACHE_LINE_SIZE - sizeof(u_int)]; } xg_sq[XG_SQ_BUCKETS];

static u_int
xg_sq_hash(const volatile void *w)
{
	return ((u_int)(((uintptr_t)w >> 3) * 2654435761u) % XG_SQ_BUCKETS);
}

static void
xg_sq_lock(const volatile void *w)
{
	volatile u_int *l = &xg_sq[xg_sq_hash(w)].lock;
	struct xg_spin s = { 0, 0, 0 };

	critical_enter();
	while (!atomic_cmpset_acq_int(l, 0, 1))
		while (*l != 0) {
			cpu_spinwait();
			xg_spin_note(&s, "sleepq bucket", NULL, NULL, (int)xg_sq_hash(w), 0);
		}
}

static void
xg_sq_unlock(const volatile void *w)
{
	atomic_store_rel_int(&xg_sq[xg_sq_hash(w)].lock, 0);
	critical_exit();
}

static unsigned long long
xg_sbt_deadline(sbintime_t abs_sbt)
{
	sbintime_t d = abs_sbt - xg_sbinuptime();
	unsigned long long iv, dl;

	if (d < 0)
		d = 0;
	nanoseconds_to_absolutetime((unsigned long long)sbttons(d), &iv);
	dl = mach_absolute_time() + iv;
	return (dl);
}

/* sleep on ev; the caller holds ev's chain lock, which is dropped here */
static int
xg_block(const volatile void *chain, const volatile void *ev, int intr, sbintime_t abs_timeo)
{
	int r;

	if (abs_timeo != 0)
		assert_wait_deadline((void *)(uintptr_t)ev, intr ? XG_THREAD_ABORTSAFE : XG_THREAD_UNINT,
		    xg_sbt_deadline(abs_timeo));
	else
		assert_wait((void *)(uintptr_t)ev, intr ? XG_THREAD_ABORTSAFE : XG_THREAD_UNINT);
	xg_sq_unlock(chain);
	r = thread_block(NULL);
	if (r == XG_THREAD_TIMED_OUT)
		return (EWOULDBLOCK);
	if (r == XG_THREAD_INTERRUPTED)
		return (EINTR);
	return (0);
}

static void
xg_wake_all(const volatile void *ev)
{
	thread_wakeup_prim((void *)(uintptr_t)ev, 0, XG_THREAD_AWAKENED);
}

static void
xg_lo_init(struct lock_object *lo, const char *name, int class, int recursable)
{
	lo->lo_name = name;
	lo->lo_flags = class | LO_INITIALIZED | (recursable ? LO_RECURSABLE : 0);
	lo->lo_data = 0;
	lo->lo_witness = NULL;
}

/* ---- mutexes ---- */
void
_mtx_init(volatile uintptr_t *c, const char *name, const char *type, int opts)
{
	struct mtx *m = mtxlock2mtx(c);

	xg_lo_init(&m->lock_object, name, (opts & MTX_SPIN) ? XG_LC_SPIN : XG_LC_MTX, opts & MTX_RECURSE);
	m->mtx_lock = MTX_UNOWNED;
}

void
_mtx_destroy(volatile uintptr_t *c)
{
	*c = MTX_DESTROYED;
}

void
mtx_sysinit(const void *arg)
{
	const struct mtx_args *a = arg;

	mtx_init((struct mtx *)a->ma_mtx, a->ma_desc, NULL, a->ma_opts);
}

void
__mtx_lock_flags(volatile uintptr_t *c, int opts, const char *file, int line)
{
	struct mtx *m = mtxlock2mtx(c);
	uintptr_t tid = (uintptr_t)curthread, v;
	struct xg_spin s = { 0, 0, 0 };

	if (atomic_cmpset_acq_ptr(c, MTX_UNOWNED, tid))
		return;
	if ((*c & ~MTX_FLAGMASK) == tid) {
		if (!(m->lock_object.lo_flags & LO_RECURSABLE) && !(opts & MTX_RECURSE))
			panic("xegfx: recursed on non-recursive mutex %s at %s:%d", m->lock_object.lo_name, file, line);
		m->mtx_recurse++;
		atomic_set_ptr(c, MTX_RECURSED);
		return;
	}
	for (;;) {
		for (int i = 0; i < XG_SPIN_TRIES; i++) {
			if (*c == MTX_UNOWNED && atomic_cmpset_acq_ptr(c, MTX_UNOWNED, tid))
				return;
			cpu_spinwait();
		}
		/*
		 * LinuxKPI's spin_lock is mtx_lock plus local_bh_disable, which pins the thread with preemption off, and
		 * Linux nests spinlocks freely: a second, contended one must spin as on Linux, because blocking with
		 * preemption off panics (N-239: guc ct send lock, then uncore->lock in fwtable_write32)
		 */
		if (curthread->td_critnest != 0) {
			xg_spin_note(&s, "mtx (critical section)", m->lock_object.lo_name, file, line, *c & ~MTX_FLAGMASK);
			continue;
		}
		xg_sq_lock(c);
		v = *c;
		if (v == MTX_UNOWNED || (!(v & MTX_WAITERS) && !atomic_cmpset_ptr(c, v, v | MTX_WAITERS))) {
			xg_sq_unlock(c);
			continue;
		}
		xg_block(c, c, 0, 0);
	}
}

void
__mtx_unlock_flags(volatile uintptr_t *c, int opts, const char *file, int line)
{
	struct mtx *m = mtxlock2mtx(c);
	uintptr_t tid = (uintptr_t)curthread;

	if (m->mtx_recurse != 0) {
		if (--m->mtx_recurse == 0)
			atomic_clear_ptr(c, MTX_RECURSED);
		return;
	}
	if (atomic_cmpset_rel_ptr(c, tid, MTX_UNOWNED))
		return;
	xg_sq_lock(c);
	atomic_store_rel_ptr(c, MTX_UNOWNED);
	xg_wake_all(c);
	xg_sq_unlock(c);
}

int
_mtx_trylock_flags_(volatile uintptr_t *c, int opts, const char *file, int line)
{
	struct mtx *m = mtxlock2mtx(c);
	uintptr_t tid = (uintptr_t)curthread;

	if ((*c & ~MTX_FLAGMASK) == tid && (m->lock_object.lo_flags & LO_RECURSABLE)) {
		m->mtx_recurse++;
		atomic_set_ptr(c, MTX_RECURSED);
		return (1);
	}
	return (atomic_cmpset_acq_ptr(c, MTX_UNOWNED, tid));
}

void
__mtx_lock_spin_flags(volatile uintptr_t *c, int opts, const char *file, int line)
{
	struct mtx *m = mtxlock2mtx(c);
	uintptr_t tid = (uintptr_t)curthread;

	spinlock_enter();
	if (atomic_cmpset_acq_ptr(c, MTX_UNOWNED, tid))
		return;
	if ((*c & ~MTX_FLAGMASK) == tid) {
		m->mtx_recurse++;
		return;
	}
	struct xg_spin s = { 0, 0, 0 };
	while (!atomic_cmpset_acq_ptr(c, MTX_UNOWNED, tid))
		while (*c != MTX_UNOWNED) {
			cpu_spinwait();
			xg_spin_note(&s, "spin mtx", m->lock_object.lo_name, file, line, *c & ~MTX_FLAGMASK);
		}
}

int
__mtx_trylock_spin_flags(volatile uintptr_t *c, int opts, const char *file, int line)
{
	uintptr_t tid = (uintptr_t)curthread;

	spinlock_enter();
	if (atomic_cmpset_acq_ptr(c, MTX_UNOWNED, tid))
		return (1);
	spinlock_exit();
	return (0);
}

void
__mtx_unlock_spin_flags(volatile uintptr_t *c, int opts, const char *file, int line)
{
	struct mtx *m = mtxlock2mtx(c);

	if (m->mtx_recurse != 0)
		m->mtx_recurse--;
	else
		atomic_store_rel_ptr(c, MTX_UNOWNED);
	spinlock_exit();
}

void
__mtx_assert(const volatile uintptr_t *c, int what, const char *file, int line)
{
}

/* the thread lock: one spin mutex for every thread shadow (xg_kern.c points td_lock here) */
struct mtx xg_thread_lock_mtx;

void
thread_lock_flags_(struct thread *td, int opts, const char *file, int line)
{
	__mtx_lock_spin_flags(&td->td_lock->mtx_lock, opts, file, line);
}

/* ---- sx ---- */
void
sx_init_flags(struct sx *sx, const char *description, int opts)
{
	xg_lo_init(&sx->lock_object, description, XG_LC_SX, opts & SX_RECURSE);
	sx->sx_lock = SX_LOCK_UNLOCKED;
}

void
sx_destroy(struct sx *sx)
{
	sx->sx_lock = SX_LOCK_DESTROYED;
}

void
sx_sysinit(const void *arg)
{
	const struct sx_args *a = arg;

	sx_init_flags(a->sa_sx, a->sa_desc, a->sa_flags);
}

int
sx_try_xlock_(struct sx *sx, const char *file, int line)
{
	uintptr_t tid = (uintptr_t)curthread;

	if (SX_OWNER(sx->sx_lock) == tid && (sx->lock_object.lo_flags & LO_RECURSABLE)) {
		sx->sx_recurse++;
		atomic_set_ptr(&sx->sx_lock, SX_LOCK_RECURSED);
		return (1);
	}
	return (atomic_cmpset_acq_ptr(&sx->sx_lock, SX_LOCK_UNLOCKED, tid));
}

int
_sx_xlock(struct sx *sx, int opts, const char *file, int line)
{
	uintptr_t tid = (uintptr_t)curthread, v;
	volatile uintptr_t *c = &sx->sx_lock;
	int e;

	if (atomic_cmpset_acq_ptr(c, SX_LOCK_UNLOCKED, tid))
		return (0);
	if (SX_OWNER(*c) == tid && !(*c & SX_LOCK_SHARED)) {
		if (!(sx->lock_object.lo_flags & LO_RECURSABLE))
			panic("xegfx: recursed on non-recursive sx %s at %s:%d", sx->lock_object.lo_name, file, line);
		sx->sx_recurse++;
		atomic_set_ptr(c, SX_LOCK_RECURSED);
		return (0);
	}
	for (;;) {
		for (int i = 0; i < XG_SPIN_TRIES; i++) {
			if (*c == SX_LOCK_UNLOCKED && atomic_cmpset_acq_ptr(c, SX_LOCK_UNLOCKED, tid))
				return (0);
			cpu_spinwait();
		}
		xg_sq_lock(c);
		v = *c;
		if (v == SX_LOCK_UNLOCKED ||
		    (!(v & SX_LOCK_EXCLUSIVE_WAITERS) && !atomic_cmpset_ptr(c, v, v | SX_LOCK_EXCLUSIVE_WAITERS))) {
			xg_sq_unlock(c);
			continue;
		}
		if ((e = xg_block(c, c, (opts & SX_INTERRUPTIBLE) != 0, 0)) != 0)
			return (e);
	}
}

void
_sx_xunlock(struct sx *sx, const char *file, int line)
{
	volatile uintptr_t *c = &sx->sx_lock;

	if (sx->sx_recurse != 0) {
		if (--sx->sx_recurse == 0)
			atomic_clear_ptr(c, SX_LOCK_RECURSED);
		return;
	}
	if (atomic_cmpset_rel_ptr(c, (uintptr_t)curthread, SX_LOCK_UNLOCKED))
		return;
	xg_sq_lock(c);
	atomic_store_rel_ptr(c, SX_LOCK_UNLOCKED);
	xg_wake_all(c);
	xg_sq_unlock(c);
}

/* readers get in whenever the lock is not held exclusively (writers can starve, readers cannot deadlock) */
int
_sx_slock(struct sx *sx, int opts, const char *file, int line)
{
	volatile uintptr_t *c = &sx->sx_lock;
	uintptr_t v;
	int e;

	for (;;) {
		v = *c;
		if (v & SX_LOCK_SHARED) {
			if (atomic_cmpset_acq_ptr(c, v, v + SX_ONE_SHARER))
				return (0);
			continue;
		}
		xg_sq_lock(c);
		v = *c;
		if ((v & SX_LOCK_SHARED) ||
		    (!(v & SX_LOCK_EXCLUSIVE_WAITERS) && !atomic_cmpset_ptr(c, v, v | SX_LOCK_EXCLUSIVE_WAITERS))) {
			xg_sq_unlock(c);
			continue;
		}
		if ((e = xg_block(c, c, (opts & SX_INTERRUPTIBLE) != 0, 0)) != 0)
			return (e);
	}
}

void
_sx_sunlock(struct sx *sx, const char *file, int line)
{
	volatile uintptr_t *c = &sx->sx_lock;
	uintptr_t v;

	for (;;) {
		v = *c;
		if (SX_SHARERS(v) == 1 && (v & SX_LOCK_WAITERS)) {
			xg_sq_lock(c);
			if (!atomic_cmpset_rel_ptr(c, v, SX_LOCK_UNLOCKED)) {
				xg_sq_unlock(c);
				continue;
			}
			xg_wake_all(c);
			xg_sq_unlock(c);
			return;
		}
		if (atomic_cmpset_rel_ptr(c, v, v - SX_ONE_SHARER))
			return;
	}
}

/* ---- rw ---- */
void
_rw_init_flags(volatile uintptr_t *c, const char *name, int opts)
{
	struct rwlock *rw = rwlock2rw(c);

	xg_lo_init(&rw->lock_object, name, XG_LC_RW, opts & RW_RECURSE);
	rw->rw_lock = RW_UNLOCKED;
}

void
_rw_destroy(volatile uintptr_t *c)
{
	*c = RW_DESTROYED;
}

void
_rw_wlock_cookie(volatile uintptr_t *c, const char *file, int line)
{
	struct rwlock *rw = rwlock2rw(c);
	uintptr_t tid = (uintptr_t)curthread, v;
	struct xg_spin s = { 0, 0, 0 };

	if (atomic_cmpset_acq_ptr(c, RW_UNLOCKED, tid))
		return;
	if (RW_OWNER(*c) == tid && !(*c & RW_LOCK_READ)) {
		rw->rw_recurse++;
		atomic_set_ptr(c, RW_LOCK_WRITER_RECURSED);
		return;
	}
	for (;;) {
		for (int i = 0; i < XG_SPIN_TRIES; i++) {
			if (*c == RW_UNLOCKED && atomic_cmpset_acq_ptr(c, RW_UNLOCKED, tid))
				return;
			cpu_spinwait();
		}
		/* inside a critical section (an RCU reader, a spin_lock) it spins, as for mutexes (N-239, N-286) */
		if (curthread->td_critnest != 0) {
			xg_spin_note(&s, "rw wlock (critical section)", rw->lock_object.lo_name, file, line, RW_OWNER(*c));
			continue;
		}
		xg_sq_lock(c);
		v = *c;
		if (v == RW_UNLOCKED ||
		    (!(v & RW_LOCK_WRITE_WAITERS) && !atomic_cmpset_ptr(c, v, v | RW_LOCK_WRITE_WAITERS))) {
			xg_sq_unlock(c);
			continue;
		}
		xg_block(c, c, 0, 0);
	}
}

void
_rw_wunlock_cookie(volatile uintptr_t *c, const char *file, int line)
{
	struct rwlock *rw = rwlock2rw(c);

	if (rw->rw_recurse != 0) {
		if (--rw->rw_recurse == 0)
			atomic_clear_ptr(c, RW_LOCK_WRITER_RECURSED);
		return;
	}
	if (atomic_cmpset_rel_ptr(c, (uintptr_t)curthread, RW_UNLOCKED))
		return;
	xg_sq_lock(c);
	atomic_store_rel_ptr(c, RW_UNLOCKED);
	xg_wake_all(c);
	xg_sq_unlock(c);
}

void
__rw_rlock(volatile uintptr_t *c, const char *file, int line)
{
	uintptr_t v;
	struct xg_spin s = { 0, 0, 0 };

	for (;;) {
		v = *c;
		if (v & RW_LOCK_READ) {
			if (atomic_cmpset_acq_ptr(c, v, v + RW_ONE_READER))
				return;
			continue;
		}
		/*
		 * Linux rwlock_t is this lock, and Linux takes it inside rcu_read_lock, which pins with preemption off:
		 * blocking there panicked WindowServer's mmap-offset lookup (N-286, drm_vma_offset_lock_lookup)
		 */
		if (curthread->td_critnest != 0) {
			xg_spin_note(&s, "rw rlock (critical section)", rwlock2rw(c)->lock_object.lo_name, file, line, RW_OWNER(v));
			cpu_spinwait();
			continue;
		}
		xg_sq_lock(c);
		v = *c;
		if ((v & RW_LOCK_READ) ||
		    (!(v & RW_LOCK_WRITE_WAITERS) && !atomic_cmpset_ptr(c, v, v | RW_LOCK_WRITE_WAITERS))) {
			xg_sq_unlock(c);
			continue;
		}
		xg_block(c, c, 0, 0);
	}
}

void
_rw_runlock_cookie(volatile uintptr_t *c, const char *file, int line)
{
	uintptr_t v;

	for (;;) {
		v = *c;
		if (RW_READERS(v) == 1 && (v & RW_LOCK_WAITERS)) {
			xg_sq_lock(c);
			if (!atomic_cmpset_rel_ptr(c, v, RW_UNLOCKED)) {
				xg_sq_unlock(c);
				continue;
			}
			xg_wake_all(c);
			xg_sq_unlock(c);
			return;
		}
		if (atomic_cmpset_rel_ptr(c, v, v - RW_ONE_READER))
			return;
	}
}

/* ---- dropping and retaking any lock around a sleep ---- */
#define XG_HOW_X 1
#define XG_HOW_S 2

static int
xg_lock_drop(struct lock_object *lo)
{
	switch (lo->lo_flags & LO_CLASSMASK) {
	case XG_LC_MTX:
		__mtx_unlock_flags(&((struct mtx *)lo)->mtx_lock, 0, __FILE__, __LINE__);
		return (XG_HOW_X);
	case XG_LC_SPIN:
		__mtx_unlock_spin_flags(&((struct mtx *)lo)->mtx_lock, 0, __FILE__, __LINE__);
		return (XG_HOW_X);
	case XG_LC_SX: {
		struct sx *sx = (struct sx *)lo;

		if (sx->sx_lock & SX_LOCK_SHARED) {
			_sx_sunlock(sx, __FILE__, __LINE__);
			return (XG_HOW_S);
		}
		_sx_xunlock(sx, __FILE__, __LINE__);
		return (XG_HOW_X);
	}
	case XG_LC_RW: {
		struct rwlock *rw = (struct rwlock *)lo;

		if (rw->rw_lock & RW_LOCK_READ) {
			_rw_runlock_cookie(&rw->rw_lock, __FILE__, __LINE__);
			return (XG_HOW_S);
		}
		_rw_wunlock_cookie(&rw->rw_lock, __FILE__, __LINE__);
		return (XG_HOW_X);
	}
	}
	panic("xegfx: sleep with lock %s of unknown class 0x%x", lo->lo_name, lo->lo_flags);
}

static void
xg_lock_take(struct lock_object *lo, int how)
{
	switch (lo->lo_flags & LO_CLASSMASK) {
	case XG_LC_MTX:
		__mtx_lock_flags(&((struct mtx *)lo)->mtx_lock, 0, __FILE__, __LINE__);
		break;
	case XG_LC_SPIN:
		__mtx_lock_spin_flags(&((struct mtx *)lo)->mtx_lock, 0, __FILE__, __LINE__);
		break;
	case XG_LC_SX:
		if (how == XG_HOW_S)
			_sx_slock((struct sx *)lo, 0, __FILE__, __LINE__);
		else
			_sx_xlock((struct sx *)lo, 0, __FILE__, __LINE__);
		break;
	case XG_LC_RW:
		if (how == XG_HOW_S)
			__rw_rlock(&((struct rwlock *)lo)->rw_lock, __FILE__, __LINE__);
		else
			_rw_wlock_cookie(&((struct rwlock *)lo)->rw_lock, __FILE__, __LINE__);
		break;
	}
}

/* ---- condition variables ---- */
void
cv_init(struct cv *cvp, const char *desc)
{
	cvp->cv_description = desc;
	cvp->cv_waiters = 0;
}

void
cv_destroy(struct cv *cvp)
{
}

static int
xg_cv_sleep(struct cv *cvp, struct lock_object *lock, int intr, sbintime_t abs_timeo, int relock)
{
	int how, r;

	xg_sq_lock(cvp);
	cvp->cv_waiters++;
	if (abs_timeo != 0)
		assert_wait_deadline(cvp, intr ? XG_THREAD_ABORTSAFE : XG_THREAD_UNINT, xg_sbt_deadline(abs_timeo));
	else
		assert_wait(cvp, intr ? XG_THREAD_ABORTSAFE : XG_THREAD_UNINT);
	xg_sq_unlock(cvp);
	how = xg_lock_drop(lock);
	r = thread_block(NULL);
	if (r != XG_THREAD_AWAKENED) {
		xg_sq_lock(cvp);
		if (cvp->cv_waiters > 0)
			cvp->cv_waiters--;
		xg_sq_unlock(cvp);
	}
	if (relock)
		xg_lock_take(lock, how);
	return (r == XG_THREAD_TIMED_OUT ? EWOULDBLOCK : r == XG_THREAD_INTERRUPTED ? EINTR : 0);
}

void
_cv_wait(struct cv *cvp, struct lock_object *lock)
{
	xg_cv_sleep(cvp, lock, 0, 0, 1);
}

void
_cv_wait_unlock(struct cv *cvp, struct lock_object *lock)
{
	xg_cv_sleep(cvp, lock, 0, 0, 0);
}

int
_cv_wait_sig(struct cv *cvp, struct lock_object *lock)
{
	return (xg_cv_sleep(cvp, lock, 1, 0, 1));
}

int
_cv_timedwait_sbt(struct cv *cvp, struct lock_object *lock, sbintime_t sbt, sbintime_t pr, int flags)
{
	return (xg_cv_sleep(cvp, lock, 0, (flags & C_ABSOLUTE) ? sbt : xg_sbinuptime() + sbt, 1));
}

int
_cv_timedwait_sig_sbt(struct cv *cvp, struct lock_object *lock, sbintime_t sbt, sbintime_t pr, int flags)
{
	return (xg_cv_sleep(cvp, lock, 1, (flags & C_ABSOLUTE) ? sbt : xg_sbinuptime() + sbt, 1));
}

void
cv_signal(struct cv *cvp)
{
	xg_sq_lock(cvp);
	if (cvp->cv_waiters > 0) {
		cvp->cv_waiters--;
		thread_wakeup_prim(cvp, 1, XG_THREAD_AWAKENED);
	}
	xg_sq_unlock(cvp);
}

void
cv_broadcastpri(struct cv *cvp, int pri)
{
	xg_sq_lock(cvp);
	if (cvp->cv_waiters > 0) {
		cvp->cv_waiters = 0;
		xg_wake_all(cvp);
	}
	xg_sq_unlock(cvp);
}

/* ---- sleep queues (LinuxKPI's wait queues and wake_up_process sit on these) ---- */
static const void *
xg_sq_event(const void *wchan, int queue)
{
	return ((const char *)wchan + queue);
}

void
sleepq_lock(const void *wchan)
{
	xg_sq_lock(wchan);
}

void
sleepq_release(const void *wchan)
{
	xg_sq_unlock(wchan);
}

void
sleepq_add(const void *wchan, struct lock_object *lock, const char *wmesg, int flags, int queue)
{
	struct thread *td = curthread;

	td->td_wchan = wchan;
	td->td_wmesg = wmesg;
	td->td_sqqueue = (u_char)queue;
	td->td_sleeptimo = 0;
	if (flags & SLEEPQ_INTERRUPTIBLE)
		td->td_flags |= TDF_SINTR;
	else
		td->td_flags &= ~TDF_SINTR;
	/* FreeBSD drops the interlock here; the caller still holds the chain lock */
	if (lock != NULL)
		panic("xegfx: sleepq_add with an interlock is not supported (%s)", wmesg);
}

void
sleepq_set_timeout_sbt(const void *wchan, sbintime_t sbt, sbintime_t pr, int flags)
{
	curthread->td_sleeptimo = (flags & C_ABSOLUTE) ? sbt : xg_sbinuptime() + sbt;
}

static int
xg_sleepq_block(const void *wchan, int timed)
{
	struct thread *td = curthread;
	int e;

	e = xg_block(wchan, xg_sq_event(wchan, td->td_sqqueue), (td->td_flags & TDF_SINTR) != 0,
	    timed ? td->td_sleeptimo : 0);
	td->td_wchan = NULL;
	td->td_flags &= ~TDF_SINTR;
	return (e);
}

void
sleepq_wait(const void *wchan, int pri)
{
	xg_sleepq_block(wchan, 0);
}

int
sleepq_wait_sig(const void *wchan, int pri)
{
	return (xg_sleepq_block(wchan, 0));
}

int
sleepq_timedwait(const void *wchan, int pri)
{
	return (xg_sleepq_block(wchan, 1));
}

int
sleepq_timedwait_sig(const void *wchan, int pri)
{
	return (xg_sleepq_block(wchan, 1));
}

void
sleepq_signal(const void *wchan, int flags, int pri, int queue)
{
	thread_wakeup_prim((void *)(uintptr_t)xg_sq_event(wchan, queue), 1, XG_THREAD_AWAKENED);
}

void
sleepq_broadcast(const void *wchan, int flags, int pri, int queue)
{
	xg_wake_all(xg_sq_event(wchan, queue));
}

/* ---- sleeping without a channel ---- */
int
pause_sbt(const char *wmesg, sbintime_t sbt, sbintime_t pr, int flags)
{
	struct thread *td = curthread;
	int e;

	xg_sq_lock(td);
	e = xg_block(td, &td->td_sleeptimo, (flags & C_CATCH) != 0, xg_sbinuptime() + sbt);
	return (e == 0 ? EWOULDBLOCK : e);
}

void
sched_relinquish(struct thread *td)
{
	thread_block(NULL);
}

/* callouts take and drop their associated lock through these */
void
xg_lock_acquire(struct lock_object *lo)
{
	xg_lock_take(lo, XG_HOW_X);
}

void
xg_lock_release(struct lock_object *lo)
{
	xg_lock_drop(lo);
}

/* ---- msleep/wakeup: the wait is asserted before the interlock is dropped, so a wakeup issued under that
 * interlock cannot be lost, and no chain lock is held while the interlock is released ---- */
int
_sleep(const void *chan, struct lock_object *lock, int pri, const char *wmesg, sbintime_t sbt, sbintime_t pr,
    int flags)
{
	int how = 0, r, intr = (pri & PCATCH) != 0;

	xg_sq_lock(chan);
	if (sbt != 0)
		assert_wait_deadline((void *)(uintptr_t)chan, intr ? XG_THREAD_ABORTSAFE : XG_THREAD_UNINT,
		    xg_sbt_deadline((flags & C_ABSOLUTE) ? sbt : xg_sbinuptime() + sbt));
	else
		assert_wait((void *)(uintptr_t)chan, intr ? XG_THREAD_ABORTSAFE : XG_THREAD_UNINT);
	xg_sq_unlock(chan);
	if (lock != NULL)
		how = xg_lock_drop(lock);
	r = thread_block(NULL);
	if (lock != NULL && !(pri & PDROP))
		xg_lock_take(lock, how);
	return (r == XG_THREAD_TIMED_OUT ? EWOULDBLOCK : r == XG_THREAD_INTERRUPTED ? EINTR : 0);
}

void
wakeup(const void *chan)
{
	xg_sq_lock(chan);
	xg_wake_all(chan);
	xg_sq_unlock(chan);
}

void
wakeup_one(const void *chan)
{
	xg_sq_lock(chan);
	thread_wakeup_prim((void *)(uintptr_t)chan, 1, XG_THREAD_AWAKENED);
	xg_sq_unlock(chan);
}
