/* XeGfx: FreeBSD's kernel.h; ticks is read from the XNU clock instead of a hardclock-updated global */
#include_next <sys/kernel.h>
#ifndef _XG_KERNEL_H_
#define _XG_KERNEL_H_
int xg_ticks(void);
#define ticks xg_ticks()
#endif
