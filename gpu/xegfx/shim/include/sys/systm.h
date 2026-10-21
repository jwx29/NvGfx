/* XeGfx: FreeBSD's systm.h with its ELF-only section attributes removed */
#include_next <sys/systm.h>
#undef __read_mostly
#define __read_mostly
