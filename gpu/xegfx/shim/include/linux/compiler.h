/* XeGfx: LinuxKPI's compiler.h; Apple clang cannot take __typeof__ of a __counted_by array, and the attribute is
 * only a bounds-check hint, so it is dropped */
#include_next <linux/compiler.h>
#undef __counted_by
#define __counted_by(_x)
