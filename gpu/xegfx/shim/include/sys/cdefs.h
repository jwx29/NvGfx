/* XeGfx: FreeBSD's cdefs.h. __GLOBL emits ".globl" for linker-set bounds (set_pcpu) that do not exist on Mach-O. */
#include_next <sys/cdefs.h>
#undef __GLOBL
#define __GLOBL(sym)
