/* XeGfx: counter(9) as one 64-bit cell per counter (xg_misc.c allocates them). FreeBSD's amd64 version adds through
 * %gs into its per-CPU area, which on XNU is XNU's own per-CPU data. */
#ifndef __MACHINE_COUNTER_H__
#define __MACHINE_COUNTER_H__
#include <sys/pcpu.h>
#include <machine/atomic.h>
#define	EARLY_COUNTER	NULL
#define	counter_u64_add_protected(c, i)	counter_u64_add(c, i)
static inline void
counter_u64_add(counter_u64_t c, int64_t inc)
{
	atomic_add_64((volatile uint64_t *)c, (uint64_t)inc);
}
static inline uint64_t
counter_u64_fetch_inline(uint64_t *p)
{
	return (atomic_load_64((volatile uint64_t *)p));
}
static inline void
counter_u64_zero_inline(counter_u64_t c)
{
	atomic_store_64((volatile uint64_t *)c, 0);
}
#endif
