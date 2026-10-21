/* XeGfx: LinuxKPI's jiffies.h; jiffies is read from the XNU clock */
#include_next <linux/jiffies.h>
#ifndef _XG_JIFFIES_H_
#define _XG_JIFFIES_H_
unsigned long xg_jiffies(void);
#undef jiffies_64
#define jiffies xg_jiffies()
#define jiffies_64 xg_jiffies()
#endif
