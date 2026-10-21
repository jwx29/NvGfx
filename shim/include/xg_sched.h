/*
 * XeGfx: what xg_sched.c offers the rest of the FreeBSD-side shim (thread registry, CPU pinning, group task queues).
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#ifndef _XG_SCHED_H_
#define _XG_SCHED_H_

struct thread;
struct gtask;
struct gtaskqueue;

/* proc0's locks and thread list, allproc, the CPUID globals; idempotent, safe before any lock is used */
void xg_sched_init(void);

/* puts a thread shadow on proc0's thread list and the tid hash (tdfind); any context, never sleeps */
void xg_sched_td_register(struct thread *td);

/* sched_pin()/sched_unpin() that really keep the XNU thread on its CPU (thread_bind) */
void xg_sched_pin(void);
void xg_sched_unpin(void);

/* group task queues: one XNU worker each; grouptaskqueue_enqueue() is safe from interrupt context */
struct gtaskqueue *xg_sched_gtq_create(const char *name);
void xg_sched_gtq_free(struct gtaskqueue *q);
int xg_sched_gtq_cancel(struct gtaskqueue *q, struct gtask *t);
void xg_sched_gtq_drain(struct gtaskqueue *q, struct gtask *t);
void xg_sched_gtq_drain_all(struct gtaskqueue *q);
#endif
