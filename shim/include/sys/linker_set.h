/* XeGfx: FreeBSD linker sets on Mach-O. Every entry goes to one section, tagged with its set name; the kext's
 * start routine walks __DATA,__xg_set to run SYSINIT and friends. Section names are limited to 16 chars, so
 * per-set sections are not possible. */
#ifndef _SYS_LINKER_SET_H_
#define _SYS_LINKER_SET_H_
#include <sys/cdefs.h>
struct xg_set_entry { const char *set; const void *sym; };
#define __MAKE_SET_QV(set, sym, qv)						\
	static const struct xg_set_entry __set_##set##_sym_##sym		\
	__attribute__((__section__("__DATA,__xg_set"), __used__)) =		\
	{ #set, (const void *)&(sym) }
#define __MAKE_SET(set, sym)	__MAKE_SET_QV(set, sym, const)
#define TEXT_SET(set, sym)	__MAKE_SET(set, sym)
#define DATA_SET(set, sym)	__MAKE_SET(set, sym)
#define DATA_WSET(set, sym)	__MAKE_SET_QV(set, sym, )
#define BSS_SET(set, sym)	__MAKE_SET(set, sym)
#define ABS_SET(set, sym)	__MAKE_SET(set, sym)
#define SET_ENTRY(set, sym)	__MAKE_SET(set, sym)
/* iteration (not used by drm-kmod or LinuxKPI): resolved at run time from the tagged section */
const struct xg_set_entry *xg_set_first(const char *set, const struct xg_set_entry *after);
#define SET_DECLARE(set, ptype)	typedef ptype __xg_set_type_##set
#define SET_FOREACH(pvar, set)							\
	for (const struct xg_set_entry *__e = xg_set_first(#set, 0);		\
	    __e && ((pvar) = (__xg_set_type_##set **)&__e->sym, 1);		\
	    __e = xg_set_first(#set, __e))
#endif
