/* XeGfx: the XNU kernel functions the FreeBSD-side shim calls, declared with plain C types so this header can be
 * included next to FreeBSD's. Prototypes match Kernel.framework (SDK 15.5). */
#ifndef _XG_XNU_H_
#define _XG_XNU_H_
typedef struct xg_xnu_thread *xg_thread_t;	/* thread_t */
typedef struct xg_xnu_call *xg_call_t;		/* thread_call_t */
typedef int xg_wait_result_t;
#define XG_THREAD_UNINT		0
#define XG_THREAD_ABORTSAFE	1
#define XG_THREAD_AWAKENED	0
#define XG_THREAD_TIMED_OUT	1
#define XG_THREAD_INTERRUPTED	2

extern xg_thread_t current_thread(void);
extern int cpu_number(void);
extern unsigned int real_ncpus;
extern unsigned long long mach_absolute_time(void);
extern void absolutetime_to_nanoseconds(unsigned long long abstime, unsigned long long *result);
extern void nanoseconds_to_absolutetime(unsigned long long nanoseconds, unsigned long long *result);
extern void clock_interval_to_deadline(unsigned int interval, unsigned int scale_factor, unsigned long long *result);
extern xg_wait_result_t assert_wait(void *event, int interruptible);
extern xg_wait_result_t assert_wait_deadline(void *event, int interruptible, unsigned long long deadline);
extern xg_wait_result_t thread_block(void *continuation);
extern int thread_wakeup_prim(void *event, int one_thread, xg_wait_result_t result);
/* spin locks: a held lck_spin keeps preemption off, the public way for a kext to stay on its CPU */
typedef struct xg_lck_grp xg_lck_grp_t;
typedef struct xg_lck_spin xg_lck_spin_t;
extern xg_lck_grp_t *lck_grp_alloc_init(const char *grp_name, void *attr);
extern xg_lck_spin_t *lck_spin_alloc_init(xg_lck_grp_t *grp, void *attr);
extern void lck_spin_lock(xg_lck_spin_t *lck);
extern void lck_spin_unlock(xg_lck_spin_t *lck);
extern int ml_set_interrupts_enabled(int enable);
extern int ml_get_interrupts_enabled(void);
extern int ml_at_interrupt_context(void);
extern void IODelay(unsigned int microseconds);
extern void IOSleep(unsigned int milliseconds);
extern void *IOMalloc(unsigned long size);
extern void *IOMallocZero(unsigned long size);
extern void IOFree(void *address, unsigned long size);
extern void *IOMallocAligned(unsigned long size, unsigned long alignment);
extern void IOFreeAligned(void *address, unsigned long size);
extern void *IOMallocContiguous(unsigned long size, unsigned long alignment, unsigned long long *physicalAddress);
extern void IOFreeContiguous(void *address, unsigned long size);
extern int kernel_thread_start(void (*continuation)(void *, int), void *parameter, xg_thread_t *new_thread);
extern void thread_deallocate(xg_thread_t thread);
extern int thread_terminate(xg_thread_t thread);
extern unsigned long long thread_tid(xg_thread_t thread);
extern void read_random(void *buffer, unsigned int numBytes);
extern int vprintf(const char *fmt, __builtin_va_list ap);
extern int vsnprintf(char *str, unsigned long size, const char *fmt, __builtin_va_list ap);
extern void kprintf(const char *fmt, ...);
extern int PE_parse_boot_argn(const char *arg_string, void *arg_ptr, int max_arg);
extern int PE_parse_boot_arg_str(const char *arg_string, char *arg_ptr, int size);
extern void panic(const char *fmt, ...) __attribute__((__noreturn__));
#define XG_TC_PRIORITY_KERNEL	1
#define XG_TC_OPTIONS_ONCE	0x1
extern xg_call_t thread_call_allocate_with_options(void (*func)(void *, void *), void *param0, int pri, unsigned int options);
extern int thread_call_enter1_delayed(xg_call_t call, void *param1, unsigned long long deadline);
extern int thread_call_cancel(xg_call_t call);
extern int thread_call_free(xg_call_t call);
#endif
