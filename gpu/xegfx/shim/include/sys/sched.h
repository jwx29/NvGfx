/* XeGfx: FreeBSD's sched.h; sched_pin()/sched_unpin() really bind the XNU thread (xg_sched.c), since only counting
 * td_pinned would let the thread migrate inside an RCU read section */
#include_next <sys/sched.h>
#ifndef _XG_SCHED_H_OVERRIDE_
#define _XG_SCHED_H_OVERRIDE_
void xg_sched_pin(void);
void xg_sched_unpin(void);
#define sched_pin()	xg_sched_pin()
#define sched_unpin()	xg_sched_unpin()
#endif
