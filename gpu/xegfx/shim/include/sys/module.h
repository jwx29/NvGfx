/* XeGfx: FreeBSD's module.h; MODULE_DEPEND without the ELF ".data" section (Mach-O puts it in __DATA anyway) */
#include_next <sys/module.h>
#undef MODULE_DEPEND
#define	MODULE_DEPEND(module, mdepend, vmin, vpref, vmax)		\
	static struct mod_depend MODULE_DEPEND_CONCAT(module, mdepend) = {	\
		vmin,							\
		vpref,							\
		vmax							\
	};								\
	MODULE_METADATA(MODULE_DEPEND_CONCAT(module, mdepend),		\
	    MDT_DEPEND, &MODULE_DEPEND_CONCAT(module, mdepend), #mdepend)
#undef MODULE_VERSION
#define	MODULE_VERSION(module, version)					\
	static struct mod_version MODULE_VERSION_CONCAT(module, version) = {	\
		version							\
	};								\
	MODULE_METADATA(MODULE_VERSION_CONCAT(module, version), MDT_VERSION,\
	    &MODULE_VERSION_CONCAT(module, version), __XSTRING(module))
