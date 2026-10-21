/*
 * XeGfx: the XNU scheduler calls xg_sched.c makes beyond xg_xnu.h, with plain C types so FreeBSD headers can sit
 * next to it. The first two are in the SDK; the rest are exported to kexts by com.apple.kernel but not in the SDK,
 * so their prototypes come from xnu's osfmk (kern/sched_prim.h, kern/processor.h).
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#ifndef _XG_SCHED_XNU_H_
#define _XG_SCHED_XNU_H_
#include "xg_xnu.h"

typedef struct xg_xnu_processor *xg_processor_t;	/* processor_t */
#define XG_PROCESSOR_NULL	((xg_processor_t)0)

/* SDK 15.5: kern/thread.h, kern/sched_prim.h */
extern void thread_set_thread_name(xg_thread_t th, const char *name);
extern int preemption_enabled(void);

/* xnu osfmk, exported but private */
extern xg_processor_t thread_bind(xg_processor_t processor);
extern xg_processor_t cpu_to_processor(int cpu);
extern xg_processor_t current_processor(void);
extern int clear_wait(xg_thread_t thread, xg_wait_result_t result);
#endif
