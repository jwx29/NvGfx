/* XeGfx: per-CPU data on XNU. FreeBSD keeps DPCPU variables in an ELF section copied per CPU and reaches the current
 * CPU's block through %gs; XNU has neither for kexts, so each DPCPU variable is an array indexed by CPU number and
 * curthread is a FreeBSD struct thread kept per XNU thread (xg_thread.c). */
#include_next <sys/pcpu.h>
#ifndef _XG_PCPU_H_
#define _XG_PCPU_H_
#define XG_MAXCPU 256
int xg_curcpu(void);
struct thread *xg_curthread(void);

/* the variable itself is the initial value; xg_dpcpu_block() returns its per-CPU copies (made on first use, from the
 * template), and every access site caches that block in a local static */
void *xg_dpcpu_block(const void *tmpl, unsigned long size);
#undef DPCPU_DECLARE
#undef DPCPU_DEFINE
#undef DPCPU_DEFINE_STATIC
#undef DPCPU_PTR
#undef DPCPU_ID_PTR
#define	DPCPU_DECLARE(t, n)	extern t DPCPU_NAME(n)
#define	DPCPU_DEFINE(t, n)	struct _hack; t DPCPU_NAME(n)
#define	DPCPU_DEFINE_STATIC(t, n)	static t DPCPU_NAME(n)
#define	DPCPU_ID_PTR(i, n) __extension__ ({					\
	static void *__xg_blk;							\
	if (__builtin_expect(__xg_blk == 0, 0))					\
		__xg_blk = xg_dpcpu_block(&DPCPU_NAME(n), sizeof(DPCPU_NAME(n)));	\
	(__typeof(&DPCPU_NAME(n)))((char *)__xg_blk + (unsigned long)(i) * sizeof(DPCPU_NAME(n)));	\
})
#define	DPCPU_PTR(n)		DPCPU_ID_PTR(xg_curcpu(), n)

#undef PCPU_GET
#undef PCPU_PTR
#undef PCPU_SET
#define	PCPU_GET(member)	XG_PCPU_##member()
#define	XG_PCPU_cpuid()		xg_curcpu()
#define	XG_PCPU_curthread()	xg_curthread()
#undef curthread
#define	curthread		xg_curthread()
#undef curcpu
#define	curcpu			xg_curcpu()
#endif
