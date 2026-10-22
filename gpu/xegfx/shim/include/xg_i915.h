/* XeGfx: what stages 4 and 5 ask i915 after its attach (gpu/xkpi/src/i915/xg_i915_exec.c). Plain C types only, so
 * the kext side can include it.
 * <wtl> project: xegfx | author: watchthelight </wtl> */
#ifndef _XG_I915_H_
#define _XG_I915_H_
#ifdef __cplusplus
extern "C" {
#endif

#define XG_I915_MAX_ENGINES	16

struct xg_i915_state {
	int		gts;		/* GTs i915 brought up (MTL/ARL: primary and media) */
	int		wedged;		/* bit n: GT n is wedged, so probe "passed" but nothing will run */
	int		guc_status;	/* enum intel_uc_fw_status of GT 0; 10 is RUNNING */
	int		huc_status;
	int		gsc_status;
	int		guc_submission;	/* 1 when requests go through the GuC */
	unsigned int	guc_ver;	/* major << 16 | minor << 8 | patch of the loaded image */
	int		engines;	/* uabi engines */
	int		render_minor;	/* -1 without a render node */
};

struct xg_i915_exec {
	int		engines;
	int		done;		/* requests that completed inside the timeout */
	char		name[XG_I915_MAX_ENGINES][16];
	long long	ns[XG_I915_MAX_ENGINES];	/* add to completion */
	int		err[XG_I915_MAX_ENGINES];	/* 0, or the negative errno of create or wait */
};

/* both return 0 with the report filled, or -ENODEV when no i915 device is bound */
int xg_i915_state(struct xg_i915_state *st);
/* one kernel-context request per uabi engine, each waited for at most timeout_ms; sleeps */
int xg_i915_exec(struct xg_i915_exec *ex, int timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
