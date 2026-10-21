/* XeGfx: the misc area's narrow bridge between the FreeBSD side (xg_misc.c) and the XNU side (xg_misc.cpp), plus
 * the hooks other areas use. Plain C types only, so it can be included on either side.
 * <wtl> project: xegfx | author: watchthelight </wtl> */
#ifndef _XG_MISC_H_
#define _XG_MISC_H_
#ifdef __cplusplus
extern "C" {
#endif

/* one-time setup (locks, the static sysctl set, tunables); the kext start calls it after xg_kern_init(), and every
 * misc entry point that needs it calls it lazily too */
void xg_misc_init(void);

/* MOD_QUIESCE then MOD_UNLOAD to every module registered through module_register_init, newest first; returns the
 * first refusal (the remaining modules stay loaded) */
int xg_misc_module_unload_all(void);

/* select/poll bridge: the fd area points these at its XNU select glue; without them selrecord/selwakeup do nothing */
struct thread;
struct selinfo;
struct xg_misc_sel_ops {
	void (*record)(struct thread *td, struct selinfo *sip);
	void (*wakeup)(struct selinfo *sip);
};
void xg_misc_set_sel_ops(const struct xg_misc_sel_ops *ops);

/* group task queues (taskqgroup_*): the sched area's grouptaskqueue_enqueue() forwards here, the queue layout is
 * private to xg_misc.c */
struct gtaskqueue;
struct gtask;
int xg_misc_gtq_enqueue(struct gtaskqueue *queue, struct gtask *task);

/* XNU side (xg_misc.cpp): read a file from the kext's Contents/Resources through OSKextRequestResource and wait for
 * it. On success *datap is an IOMalloc() copy of *lenp bytes (free it with IOFree). Returns 0 or an errno value.
 * Sleeps: never call it with a spin lock held, or from the kext start routine (kernelmanagerd answers only after
 * the start routine has returned). */
int xg_misc_fw_request(const char *name, void **datap, unsigned long *lenp, unsigned int timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
