/*
 * XeGfx: FreeBSD taskqueues and callouts on XNU. A taskqueue is a list run by its own XNU kernel threads; a callout
 * is a thread_call (kept in the callout's unused link field) that takes the callout's lock before it runs, and is
 * skipped when it was stopped while the caller held that lock, as callout(9) promises.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/taskqueue.h>
#include <sys/callout.h>
#include <sys/time.h>
#include "xg_xnu.h"

sbintime_t xg_sbinuptime(void);
void xg_lock_acquire(struct lock_object *lo);
void xg_lock_release(struct lock_object *lo);

static MALLOC_DEFINE(M_XGTQ, "xgtq", "taskqueues");

#define XG_TQ_THREADS 8
struct taskqueue {
	STAILQ_HEAD(, task) tq_queue;
	struct mtx tq_mutex;
	struct cv tq_cv;		/* work arrived */
	struct cv tq_idle;		/* a task finished */
	const char *tq_name;
	struct task *tq_running[XG_TQ_THREADS];
	struct thread *tq_td[XG_TQ_THREADS];
	int tq_nthreads;
	int tq_alive;
	int tq_exit;
};

struct taskqueue *taskqueue_thread;

static struct taskqueue *
xg_tq_create(const char *name, int mflags)
{
	struct taskqueue *tq = malloc(sizeof(*tq), M_XGTQ, mflags | M_ZERO);

	if (tq == NULL)
		return (NULL);
	STAILQ_INIT(&tq->tq_queue);
	mtx_init(&tq->tq_mutex, name, NULL, MTX_DEF);
	cv_init(&tq->tq_cv, name);
	cv_init(&tq->tq_idle, name);
	tq->tq_name = name;
	return (tq);
}

struct taskqueue *
taskqueue_create(const char *name, int mflags, taskqueue_enqueue_fn enqueue, void *context)
{
	return (xg_tq_create(name, mflags));
}

struct taskqueue *
taskqueue_create_fast(const char *name, int mflags, taskqueue_enqueue_fn enqueue, void *context)
{
	return (xg_tq_create(name, mflags));
}

void
taskqueue_thread_enqueue(void *context)
{
}

struct xg_tq_arg { struct taskqueue *tq; int slot; };

static void
xg_tq_worker(void *arg, int wr)
{
	struct xg_tq_arg a = *(struct xg_tq_arg *)arg;
	struct taskqueue *tq = a.tq;
	struct task *t;
	int pending;

	free(arg, M_XGTQ);
	mtx_lock(&tq->tq_mutex);
	tq->tq_td[a.slot] = curthread;
	tq->tq_alive++;
	for (;;) {
		while (STAILQ_EMPTY(&tq->tq_queue) && !tq->tq_exit)
			cv_wait(&tq->tq_cv, &tq->tq_mutex);
		if (tq->tq_exit)
			break;
		t = STAILQ_FIRST(&tq->tq_queue);
		STAILQ_REMOVE_HEAD(&tq->tq_queue, ta_link);
		pending = t->ta_pending;
		t->ta_pending = 0;
		tq->tq_running[a.slot] = t;
		mtx_unlock(&tq->tq_mutex);
		t->ta_func(t->ta_context, pending);
		mtx_lock(&tq->tq_mutex);
		tq->tq_running[a.slot] = NULL;
		cv_broadcast(&tq->tq_idle);
	}
	tq->tq_td[a.slot] = NULL;
	tq->tq_alive--;
	cv_broadcast(&tq->tq_idle);
	mtx_unlock(&tq->tq_mutex);
	thread_terminate(current_thread());
}

int
taskqueue_start_threads(struct taskqueue **tqp, int count, int pri, const char *name, ...)
{
	struct taskqueue *tq = *tqp;
	xg_thread_t th;

	for (int i = 0; i < count && tq->tq_nthreads < XG_TQ_THREADS; i++) {
		struct xg_tq_arg *a = malloc(sizeof(*a), M_XGTQ, M_WAITOK);

		a->tq = tq;
		a->slot = tq->tq_nthreads++;
		if (kernel_thread_start(xg_tq_worker, a, &th) != 0) {
			free(a, M_XGTQ);
			tq->tq_nthreads--;
			return (ENOMEM);
		}
		thread_deallocate(th);
	}
	return (0);
}

static int
xg_tq_running(struct taskqueue *tq, struct task *t)
{
	for (int i = 0; i < tq->tq_nthreads; i++)
		if (tq->tq_running[i] == t)
			return (1);
	return (0);
}

int
taskqueue_enqueue_flags(struct taskqueue *tq, struct task *t, int flags)
{
	mtx_lock(&tq->tq_mutex);
	if (t->ta_pending != 0) {
		if (flags & TASKQUEUE_FAIL_IF_PENDING) {
			mtx_unlock(&tq->tq_mutex);
			return (EEXIST);
		}
		if (t->ta_pending < USHRT_MAX)
			t->ta_pending++;
		mtx_unlock(&tq->tq_mutex);
		return (0);
	}
	if ((flags & TASKQUEUE_FAIL_IF_CANCELING) && xg_tq_running(tq, t)) {
		/* fine to queue again while running; FreeBSD only refuses while a cancel is in progress */
	}
	STAILQ_INSERT_TAIL(&tq->tq_queue, t, ta_link);
	t->ta_pending = 1;
	cv_signal(&tq->tq_cv);
	mtx_unlock(&tq->tq_mutex);
	return (0);
}

int
taskqueue_enqueue(struct taskqueue *tq, struct task *t)
{
	return (taskqueue_enqueue_flags(tq, t, 0));
}

int
taskqueue_cancel(struct taskqueue *tq, struct task *t, u_int *pendp)
{
	int busy;

	mtx_lock(&tq->tq_mutex);
	if (t->ta_pending != 0)
		STAILQ_REMOVE(&tq->tq_queue, t, task, ta_link);
	if (pendp != NULL)
		*pendp = t->ta_pending;
	t->ta_pending = 0;
	busy = xg_tq_running(tq, t);
	mtx_unlock(&tq->tq_mutex);
	return (busy ? EBUSY : 0);
}

void
taskqueue_drain(struct taskqueue *tq, struct task *t)
{
	mtx_lock(&tq->tq_mutex);
	while (t->ta_pending != 0 || xg_tq_running(tq, t))
		cv_wait(&tq->tq_idle, &tq->tq_mutex);
	mtx_unlock(&tq->tq_mutex);
}

static int
xg_tq_busy(struct taskqueue *tq)
{
	for (int i = 0; i < tq->tq_nthreads; i++)
		if (tq->tq_running[i] != NULL)
			return (1);
	return (!STAILQ_EMPTY(&tq->tq_queue));
}

void
taskqueue_drain_all(struct taskqueue *tq)
{
	mtx_lock(&tq->tq_mutex);
	while (xg_tq_busy(tq))
		cv_wait(&tq->tq_idle, &tq->tq_mutex);
	mtx_unlock(&tq->tq_mutex);
}

int
taskqueue_poll_is_busy(struct taskqueue *tq, struct task *t)
{
	int r;

	mtx_lock(&tq->tq_mutex);
	r = t->ta_pending != 0 || xg_tq_running(tq, t);
	mtx_unlock(&tq->tq_mutex);
	return (r);
}

int
taskqueue_member(struct taskqueue *tq, struct thread *td)
{
	for (int i = 0; i < tq->tq_nthreads; i++)
		if (tq->tq_td[i] == td)
			return (1);
	return (0);
}

void
taskqueue_free(struct taskqueue *tq)
{
	mtx_lock(&tq->tq_mutex);
	tq->tq_exit = 1;
	cv_broadcast(&tq->tq_cv);
	while (tq->tq_alive > 0)
		cv_wait(&tq->tq_idle, &tq->tq_mutex);
	mtx_unlock(&tq->tq_mutex);
	cv_destroy(&tq->tq_cv);
	cv_destroy(&tq->tq_idle);
	mtx_destroy(&tq->tq_mutex);
	free(tq, M_XGTQ);
}

/* the system "thread" taskqueue FreeBSD starts at boot */
void
xg_task_init(void)
{
	taskqueue_thread = xg_tq_create("thread taskq", M_WAITOK);
	taskqueue_start_threads(&taskqueue_thread, 1, PWAIT, "thread taskq");
}

/* ---- callouts ---- */
#define XG_CALL(c)	(*(xg_call_t *)&(c)->c_links)

static void
xg_callout_fire(void *p0, void *p1)
{
	struct callout *c = p0;
	callout_func_t *fn;
	void *arg;

	if (c->c_lock != NULL)
		xg_lock_acquire(c->c_lock);
	/* stopped (or rescheduled to a later time) after this run was queued: skip it */
	if (!(c->c_iflags & CALLOUT_PENDING) || (uintptr_t)p1 != (uintptr_t)c->c_time) {
		if (c->c_lock != NULL)
			xg_lock_release(c->c_lock);
		return;
	}
	c->c_iflags &= ~CALLOUT_PENDING;
	fn = c->c_func;
	arg = c->c_arg;
	fn(arg);
	if (c->c_lock != NULL && !(c->c_flags & CALLOUT_RETURNUNLOCKED))
		xg_lock_release(c->c_lock);
}

void
callout_init(struct callout *c, int mpsafe)
{
	bzero(c, sizeof(*c));
	XG_CALL(c) = thread_call_allocate_with_options(xg_callout_fire, c, XG_TC_PRIORITY_KERNEL, 0);
	if (XG_CALL(c) == NULL)
		panic("xegfx: no thread call for a callout");
	if (mpsafe)
		c->c_iflags |= CALLOUT_MPSAFE;
}

void
_callout_init_lock(struct callout *c, struct lock_object *lock, int flags)
{
	callout_init(c, 1);
	c->c_lock = lock;
	c->c_flags |= flags & (CALLOUT_RETURNUNLOCKED | CALLOUT_SHAREDLOCK);
}

int
callout_reset_sbt_on(struct callout *c, sbintime_t sbt, sbintime_t prec, callout_func_t *func, void *arg,
    int cpu, int flags)
{
	sbintime_t when = (flags & C_ABSOLUTE) ? sbt : xg_sbinuptime() + sbt;
	sbintime_t now = xg_sbinuptime();
	unsigned long long iv;
	int was;

	was = (c->c_iflags & CALLOUT_PENDING) != 0;
	if (was)
		thread_call_cancel(XG_CALL(c));
	c->c_func = func;
	c->c_arg = arg;
	if (when <= c->c_time)	/* keep the run token unique even for the same deadline */
		when = c->c_time + 1;
	c->c_time = when;
	c->c_iflags |= CALLOUT_PENDING;
	c->c_flags |= CALLOUT_ACTIVE;
	nanoseconds_to_absolutetime((unsigned long long)sbttons(when > now ? when - now : 0), &iv);
	thread_call_enter1_delayed(XG_CALL(c), (void *)(uintptr_t)when, mach_absolute_time() + iv);
	return (was);
}

int
_callout_stop_safe(struct callout *c, int flags)
{
	int was = (c->c_iflags & CALLOUT_PENDING) != 0;

	c->c_iflags &= ~CALLOUT_PENDING;
	c->c_flags &= ~CALLOUT_ACTIVE;
	if (XG_CALL(c) == NULL)
		return (0);
	if (flags & CS_DRAIN)
		thread_call_cancel_wait(XG_CALL(c));
	else
		thread_call_cancel(XG_CALL(c));
	return (was);
}
