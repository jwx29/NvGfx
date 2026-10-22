// SPDX-License-Identifier: MIT
/*
 * XeGfx stages 4 and 5: after i915 attaches, report whether its GTs are usable (probe returns 0 even when GuC
 * loading failed and the GT was wedged), then run one kernel-context request on every uabi engine (stage 5), then
 * have every engine write a dword the CPU can read back (stage 6).
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <linux/pci.h>
#include <linux/string.h>

#include "i915_drv.h"
#include "i915_request.h"
#include "gt/intel_engine.h"
#include "gt/intel_engine_pm.h"
#include "gt/intel_gpu_commands.h"
#include "gt/intel_ring.h"
#include "gt/intel_gt.h"
#include "gt/uc/intel_uc.h"

#include "xg_i915.h"

static struct drm_i915_private *
xg_i915_find(struct pci_dev **pdevp)
{
	struct pci_dev *pdev = NULL;

	while ((pdev = lkpi_pci_get_device(PCI_VENDOR_ID_INTEL, PCI_ANY_ID, pdev)) != NULL)
		if (pdev_to_i915(pdev) != NULL) {
			*pdevp = pdev;
			return (pdev_to_i915(pdev));
		}
	return (NULL);
}

int
xg_i915_state(struct xg_i915_state *st)
{
	struct drm_i915_private *i915;
	struct intel_engine_cs *engine;
	struct intel_uc_fw_ver *v;
	struct pci_dev *pdev;
	struct intel_gt *gt;
	unsigned int id;

	memset(st, 0, sizeof(*st));
	st->render_minor = -1;
	if ((i915 = xg_i915_find(&pdev)) == NULL)
		return (-ENODEV);
	for_each_gt(gt, i915, id) {
		st->gts++;
		if (intel_gt_is_wedged(gt))
			st->wedged |= 1 << id;
	}
	gt = to_gt(i915);
	st->guc_status = __intel_uc_fw_status(&gt->uc.guc.fw);
	st->huc_status = __intel_uc_fw_status(&gt->uc.huc.fw);
	st->gsc_status = __intel_uc_fw_status(&gt->uc.gsc.fw);
	st->guc_submission = intel_uc_uses_guc_submission(&gt->uc);
	v = &gt->uc.guc.fw.file_selected.ver;
	st->guc_ver = v->major << 16 | (v->minor & 0xff) << 8 | (v->patch & 0xff);
	for_each_uabi_engine(engine, i915)
		st->engines++;
	if (i915->drm.render != NULL)
		st->render_minor = i915->drm.render->index;
	pci_dev_put(pdev);
	return (0);
}

int
xg_i915_exec(struct xg_i915_exec *ex, int timeout_ms)
{
	struct drm_i915_private *i915;
	struct intel_engine_cs *engine;
	struct i915_request *rq;
	struct pci_dev *pdev;
	ktime_t t0;
	long t;
	int n;

	memset(ex, 0, sizeof(*ex));
	if ((i915 = xg_i915_find(&pdev)) == NULL)
		return (-ENODEV);
	for_each_uabi_engine(engine, i915) {
		if ((n = ex->engines) >= XG_I915_MAX_ENGINES)
			break;
		ex->engines++;
		strscpy(ex->name[n], engine->name, sizeof(ex->name[n]));
		t0 = ktime_get();
		rq = intel_engine_create_kernel_request(engine);
		if (IS_ERR(rq)) {
			ex->err[n] = PTR_ERR(rq);
			continue;
		}
		i915_request_get(rq);
		i915_request_add(rq);
		t = i915_request_wait(rq, 0, msecs_to_jiffies(timeout_ms));
		ex->ns[n] = ktime_to_ns(ktime_sub(ktime_get(), t0));
		i915_request_put(rq);
		if (t < 0)
			ex->err[n] = (int)t;
		else
			ex->done++;
	}
	pci_dev_put(pdev);
	return (0);
}

/* a slot past i915's own status page users (I915_GEM_HWS_SCRATCH is only written on gen2-6) */
#define XG_HWS_SLOT	(I915_GEM_HWS_SCRATCH + 1)

int
xg_i915_store(struct xg_i915_store *sd, int timeout_ms)
{
	struct drm_i915_private *i915;
	struct intel_engine_cs *engine;
	struct i915_request *rq;
	struct pci_dev *pdev;
	u32 *cs, *slot;
	long t;
	int n;

	memset(sd, 0, sizeof(*sd));
	if ((i915 = xg_i915_find(&pdev)) == NULL)
		return (-ENODEV);
	for_each_uabi_engine(engine, i915) {
		if (!intel_engine_can_store_dword(engine))
			continue;
		if ((n = sd->engines) >= XG_I915_MAX_ENGINES)
			break;
		sd->engines++;
		strscpy(sd->name[n], engine->name, sizeof(sd->name[n]));
		sd->want[n] = 0x58670000 | (n << 8) | engine->uabi_class;
		slot = &engine->status_page.addr[XG_HWS_SLOT];
		intel_write_status_page(engine, XG_HWS_SLOT, 0);
		rq = intel_engine_create_kernel_request(engine);
		if (IS_ERR(rq)) {
			sd->err[n] = PTR_ERR(rq);
			continue;
		}
		cs = intel_ring_begin(rq, 4);
		if (IS_ERR(cs)) {
			sd->err[n] = PTR_ERR(cs);
			i915_request_add(rq);
			continue;
		}
		*cs++ = MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT;
		*cs++ = i915_ggtt_offset(engine->status_page.vma) + XG_HWS_SLOT * sizeof(u32);
		*cs++ = 0;
		*cs++ = sd->want[n];
		intel_ring_advance(rq, cs);
		i915_request_get(rq);
		i915_request_add(rq);
		t = i915_request_wait(rq, 0, msecs_to_jiffies(timeout_ms));
		i915_request_put(rq);
		drm_clflush_virt_range(slot, sizeof(*slot));
		sd->got[n] = intel_read_status_page(engine, XG_HWS_SLOT);
		if (t < 0)
			sd->err[n] = (int)t;
		else if (sd->got[n] != sd->want[n])
			sd->err[n] = -EIO;
		else
			sd->ok++;
	}
	pci_dev_put(pdev);
	return (0);
}
