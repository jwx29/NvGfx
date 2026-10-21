/*
 * XeGfx: FreeBSD kernel basics on XNU. curthread (a FreeBSD struct thread kept per XNU thread), CPU numbers,
 * per-CPU data blocks, critical sections, the clock globals and boot-arg tunables in place of the kernel environment.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/smp.h>
#include <sys/pcpu.h>
#include <sys/time.h>
#include <sys/malloc.h>
#include <machine/atomic.h>
#include "xg_xnu.h"

int hz = 1000;
sbintime_t tick_sbt = SBT_1S / 1000;
int cold = 0;
int kdb_active = 0;
bool scheduler_stopped = false;
const char *panicstr = NULL;
int mp_ncpus = 1;
u_int mp_maxid = 0;
struct proc proc0;
extern struct mtx xg_thread_lock_mtx;
void xg_sched_td_register(struct thread *td);
static struct ucred xg_cred0;
static unsigned long long xg_boot_abs;

/* each shadow is followed by its thread's own lck_spin, held for the length of a critical section */
#define XG_TD_CRIT(td)	(*(xg_lck_spin_t **)((char *)(td) + sizeof(struct thread)))
static xg_lck_grp_t *xg_lck_grp;

/* thread shadows: open addressing on the XNU thread pointer, entries are never removed (see xg_thread_reap) */
#define XG_TD_SLOTS 4096
static struct { void *key; struct thread *td; } xg_td[XG_TD_SLOTS];
static volatile u_int xg_td_lock;
static lwpid_t xg_next_tid = 100000;

static void
xg_spin_lock(volatile u_int *l)
{
	while (!atomic_cmpset_acq_int(l, 0, 1))
		cpu_spinwait();
}

static void
xg_spin_unlock(volatile u_int *l)
{
	atomic_store_rel_int(l, 0);
}

int
xg_curcpu(void)
{
	return (cpu_number());
}

struct thread *
xg_curthread(void)
{
	void *k = current_thread();
	u_int h = (u_int)(((uintptr_t)k >> 4) * 2654435761u) % XG_TD_SLOTS;
	struct thread *td;
	int en;

	/* lock-free lookup: slots are only ever filled, never cleared */
	for (u_int i = 0; i < XG_TD_SLOTS; i++) {
		u_int s = (h + i) % XG_TD_SLOTS;
		void *sk = atomic_load_acq_ptr((volatile uintptr_t *)&xg_td[s].key) ? xg_td[s].key : NULL;
		if (sk == k)
			return (xg_td[s].td);
		if (sk == NULL)
			break;
	}
	/* first FreeBSD-side call on this XNU thread: make its shadow */
	td = IOMallocZero(sizeof(*td) + sizeof(xg_lck_spin_t *));
	if (td == NULL)
		panic("xegfx: no memory for a thread shadow");
	if (xg_lck_grp == NULL)
		xg_lck_grp = lck_grp_alloc_init("xegfx", NULL);
	if ((XG_TD_CRIT(td) = lck_spin_alloc_init(xg_lck_grp, NULL)) == NULL)
		panic("xegfx: no spin lock for a thread shadow");
	td->td_proc = &proc0;
	td->td_ucred = &xg_cred0;
	td->td_lock = &xg_thread_lock_mtx;
	td->td_tid = atomic_fetchadd_int((volatile u_int *)&xg_next_tid, 1);
	td->td_priority = PUSER;
	strlcpy(td->td_name, "xnu", sizeof(td->td_name));
	en = ml_set_interrupts_enabled(0);
	xg_spin_lock(&xg_td_lock);
	for (u_int i = 0; i < XG_TD_SLOTS; i++) {
		u_int s = (h + i) % XG_TD_SLOTS;
		if (xg_td[s].key == k) {	/* raced with ourselves through an interrupt: keep the first */
			IOFree(td, sizeof(*td) + sizeof(xg_lck_spin_t *));	/* its lck_spin is leaked: rare race */
			td = xg_td[s].td;
			break;
		}
		if (xg_td[s].key == NULL) {
			xg_td[s].td = td;
			atomic_store_rel_ptr((volatile uintptr_t *)&xg_td[s].key, (uintptr_t)k);
			break;
		}
		if (i == XG_TD_SLOTS - 1)
			panic("xegfx: thread shadow table full");
	}
	xg_spin_unlock(&xg_td_lock);
	ml_set_interrupts_enabled(en);
	xg_sched_td_register(td);	/* tid lookup (tdfind) and proc0's thread list */
	return (td);
}

void
thread_reap_barrier(void)
{
}

/* per-CPU copies of a DPCPU variable, made on first use from the variable's initial value */
#define XG_DPCPU_MAX 128
static struct { const void *tmpl; void *blk; } xg_dpcpu[XG_DPCPU_MAX];
static volatile u_int xg_dpcpu_lock;

void *
xg_dpcpu_block(const void *tmpl, unsigned long size)
{
	void *blk = NULL;
	int i;

	xg_spin_lock(&xg_dpcpu_lock);
	for (i = 0; i < XG_DPCPU_MAX && xg_dpcpu[i].tmpl != NULL; i++)
		if (xg_dpcpu[i].tmpl == tmpl) {
			blk = xg_dpcpu[i].blk;
			break;
		}
	xg_spin_unlock(&xg_dpcpu_lock);
	if (blk != NULL)
		return (blk);
	blk = IOMallocZero(size * XG_MAXCPU);
	if (blk == NULL)
		panic("xegfx: no memory for per-CPU data");
	for (int c = 0; c < XG_MAXCPU; c++)
		memcpy((char *)blk + c * size, tmpl, size);
	xg_spin_lock(&xg_dpcpu_lock);
	for (i = 0; i < XG_DPCPU_MAX && xg_dpcpu[i].tmpl != NULL; i++)
		if (xg_dpcpu[i].tmpl == tmpl)
			break;
	if (i == XG_DPCPU_MAX)
		panic("xegfx: too many DPCPU variables");
	if (xg_dpcpu[i].tmpl == tmpl) {
		IOFree(blk, size * XG_MAXCPU);
		blk = xg_dpcpu[i].blk;
	} else {
		xg_dpcpu[i].blk = blk;
		xg_dpcpu[i].tmpl = tmpl;
	}
	xg_spin_unlock(&xg_dpcpu_lock);
	return (blk);
}

/* critical sections: no preemption, so the current CPU (and its DPCPU data) stays put. Holding the thread's own,
 * never contended lck_spin is how a kext turns preemption off; sleeping inside one panics, as it should. */
void
critical_enter_KBI(void)
{
	struct thread *td = curthread;

	if (td->td_critnest++ == 0)
		lck_spin_lock(XG_TD_CRIT(td));
}

void
critical_exit_KBI(void)
{
	struct thread *td = curthread;

	if (--td->td_critnest == 0)
		lck_spin_unlock(XG_TD_CRIT(td));
}

void
spinlock_enter(void)
{
	struct thread *td = curthread;

	if (td->td_md.md_spinlock_count++ == 0)
		td->td_md.md_saved_flags = ml_set_interrupts_enabled(0);
	critical_enter_KBI();
}

void
spinlock_exit(void)
{
	struct thread *td = curthread;

	critical_exit_KBI();
	if (--td->td_md.md_spinlock_count == 0)
		ml_set_interrupts_enabled((int)td->td_md.md_saved_flags);
}

/* time: FreeBSD's ticks and LinuxKPI's jiffies are read through these (shim sys/kernel.h, linux/jiffies.h) */
static unsigned long long
xg_uptime_ns(void)
{
	unsigned long long ns;

	absolutetime_to_nanoseconds(mach_absolute_time() - xg_boot_abs, &ns);
	return (ns);
}

int
xg_ticks(void)
{
	return ((int)(xg_uptime_ns() / (1000000000ull / hz)));
}

unsigned long
xg_jiffies(void)
{
	return ((unsigned long)(xg_uptime_ns() / (1000000000ull / hz)));
}

sbintime_t
xg_sbinuptime(void)
{
	return (nstosbt(xg_uptime_ns()));
}

void
getnanouptime(struct timespec *ts)
{
	unsigned long long ns = xg_uptime_ns();

	ts->tv_sec = ns / 1000000000ull;
	ts->tv_nsec = ns % 1000000000ull;
}

void
DELAY(int usec)
{
	if (usec > 0)
		IODelay((unsigned)usec);
}

/* tunables come from boot-args: "hw.i915kms.enable_guc=2" is passed as i915kms.enable_guc=2 */
static const char *
xg_tunable(const char *name)
{
	if (strncmp(name, "hw.", 3) == 0)
		return (name + 3);
	return (name);
}

/*
 * debug.xegfx_env (set by the xe-guard before a stage) overrides boot-args, so one boot can try several settings:
 * space-separated name=value pairs under the same names as the boot-args.
 */
extern char xg_env[];

static int
xg_env_lookup(const char *name, char *buf, size_t len)
{
	const char *key = xg_tunable(name), *p = xg_env;
	size_t kl = strlen(key), n;

	while (*p) {
		while (*p == ' ')
			p++;
		if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
			p += kl + 1;
			for (n = 0; n + 1 < len && p[n] && p[n] != ' '; n++)
				buf[n] = p[n];
			buf[n] = 0;
			return (1);
		}
		while (*p && *p != ' ')
			p++;
	}
	return (0);
}

int
getenv_int(const char *name, int *data)
{
	char buf[32];
	int v;

	if (xg_env_lookup(name, buf, sizeof(buf))) {
		*data = (int)strtol(buf, NULL, 0);
		return (1);
	}
	if (!PE_parse_boot_argn(xg_tunable(name), &v, sizeof(v)))
		return (0);
	*data = v;
	return (1);
}

int
getenv_long(const char *name, long *data)
{
	char buf[32];
	long v;

	if (xg_env_lookup(name, buf, sizeof(buf))) {
		*data = strtol(buf, NULL, 0);
		return (1);
	}
	if (!PE_parse_boot_argn(xg_tunable(name), &v, sizeof(v)))
		return (0);
	*data = v;
	return (1);
}

char *
kern_getenv(const char *name)
{
	char buf[128];
	char *s;
	size_t n;

	/* the _str variant: PE_parse_boot_argn stores a numeric value in binary */
	if (!xg_env_lookup(name, buf, sizeof(buf)) && !PE_parse_boot_arg_str(xg_tunable(name), buf, sizeof(buf)))
		return (NULL);
	n = strnlen(buf, sizeof(buf) - 1) + 1;
	s = malloc(n, M_TEMP, M_WAITOK);
	memcpy(s, buf, n - 1);
	s[n - 1] = 0;
	return (s);
}

void
freeenv(char *env)
{
	free(env, M_TEMP);
}

void
arc4random_buf(void *p, size_t n)
{
	read_random(p, (unsigned)n);
}

uint32_t
arc4random_uniform(uint32_t bound)
{
	uint32_t r, min;

	if (bound < 2)
		return (0);
	min = -bound % bound;
	do
		read_random(&r, sizeof(r));
	while (r < min);
	return (r % bound);
}

/* the shim's start: clock base and CPU counts, before any SYSINIT runs */
void
xg_kern_init(void)
{
	xg_boot_abs = mach_absolute_time();
	mp_ncpus = (int)real_ncpus;
	mp_maxid = mp_ncpus - 1;
	if (mp_ncpus > XG_MAXCPU)
		panic("xegfx: %d CPUs, XG_MAXCPU is %d", mp_ncpus, XG_MAXCPU);
	CPU_ZERO(&all_cpus);
	for (int i = 0; i < mp_ncpus; i++)
		CPU_SET(i, &all_cpus);
	proc0.p_pid = 0;
	strlcpy(proc0.p_comm, "kernel", sizeof(proc0.p_comm));
}
