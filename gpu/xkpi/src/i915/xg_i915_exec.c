// SPDX-License-Identifier: MIT
/*
 * XeGfx stages 4 and 5: after i915 attaches, report whether its GTs are usable (probe returns 0 even when GuC
 * loading failed and the GT was wedged), then run one kernel-context request on every uabi engine (stage 5), then
 * have every engine write a dword the CPU can read back (stage 6).
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <linux/pci.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/err.h>

#include "i915_drv.h"
#include "i915_request.h"
#include "gt/intel_engine.h"
#include "gt/intel_engine_pm.h"
#include "gt/intel_gt_pm.h"
#include "gt/intel_gpu_commands.h"
#include "gt/intel_ring.h"
#include "gt/intel_gt.h"
#include "gt/uc/intel_uc.h"
#include "gt/intel_gtt.h"
#include "i915_gem_gtt.h"
#include "i915_gem.h"
#include "i915_vma.h"
#include "gem/i915_gem_object.h"

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
xg_i915_gtawake(int level)
{
	struct drm_i915_private *i915;
	struct pci_dev *pdev;
	struct intel_gt *gt;
	unsigned int id;
	int n = 0;

	if (level <= 0)
		return (0);
	if ((i915 = xg_i915_find(&pdev)) == NULL)
		return (-ENODEV);
	/* what i915's forcewake_user debugfs file does on open; never released, the A/B runs to the next restart */
	for_each_gt(gt, i915, id) {
		atomic_inc(&gt->user_wakeref);
		intel_gt_pm_get_untracked(gt);
		if (level >= 2)
			intel_uncore_forcewake_user_get(gt->uncore);
		n++;
	}
	pci_dev_put(pdev);
	printf("xegfx: gtawake %d held on %d GTs\n", level, n);
	return (n);
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

/*
 * With display disabled i915 never takes over the firmware's plane, so the GGTT range the boot surface scans out of
 * is free for the next pin: once WindowServer composites into it (X6), a context or ring placed there means our
 * frame copy writes over GPU state (N-251, N-255 froze about a minute in). Reserve it, PTEs left as they are.
 */
int
xg_i915_reserve_bootfb(unsigned long long offset, unsigned long long size)
{
	static struct drm_mm_node node;
	struct drm_i915_private *i915;
	struct pci_dev *pdev;
	struct i915_ggtt *ggtt;
	int err;

	if ((i915 = xg_i915_find(&pdev)) == NULL)
		return (-ENODEV);
	if (drm_mm_node_allocated(&node))
		return (0);
	ggtt = to_gt(i915)->ggtt;
	offset = round_down(offset, I915_GTT_PAGE_SIZE);
	size = round_up(size, I915_GTT_PAGE_SIZE);
	mutex_lock(&ggtt->vm.mutex);
	err = i915_gem_gtt_reserve(&ggtt->vm, NULL, &node, size, offset, I915_COLOR_UNEVICTABLE, PIN_NOEVICT);
	mutex_unlock(&ggtt->vm.mutex);
	return (err);
}

/*
 * A real HDMI B scanout allocation owned by XeGfx's already-initialized
 * i915 GEM/GGTT allocator. It is deliberately NOT connected to a plane yet.
 * Using the allocator (rather than hand-writing GGTT PTEs) ensures it
 * respects xg_i915_reserve_bootfb() and all other i915 placements.
 *
 * Single owner + mutex: no stale kernel pointers may be returned once
 * RELEASE is requested. A later real HDMI framebuffer backend must keep
 * this lease alive until it has quiesced the plane, VBL and copy paths.
 * No access from userspace directly: XeGfxResident enforces root.
 */
static DEFINE_MUTEX(xg_hdmi_surface_lock);
static struct {
	struct drm_i915_gem_object *obj;
	struct i915_vma *vma;
	void *cpu;
	u64 offset;
	u64 size;
} xg_hdmi_surface;

static void xg_hdmi_surface_free_locked(void)
{
	if (xg_hdmi_surface.obj) {
		if (xg_hdmi_surface.cpu)
			i915_gem_object_unpin_map(xg_hdmi_surface.obj);
		if (xg_hdmi_surface.vma)
			i915_vma_unpin(xg_hdmi_surface.vma);
		i915_gem_object_put(xg_hdmi_surface.obj);
	}
	memset(&xg_hdmi_surface, 0, sizeof(xg_hdmi_surface));
}

void xg_i915_hdmi_surface_cleanup(void)
{
	mutex_lock(&xg_hdmi_surface_lock);
	xg_hdmi_surface_free_locked();
	mutex_unlock(&xg_hdmi_surface_lock);
}

int xg_i915_hdmi_surface_manage(struct xg_i915_hdmi_surface *request)
{
	struct drm_i915_gem_object *obj = NULL;
	struct i915_vma *vma = NULL;
	struct drm_i915_private *i915;
	struct pci_dev *pdev;
	u64 bytes = (u64)1920u * 1080u * 4u;
	void *cpu = NULL;
	int err = 0;

	if (!request || request->abi != XG_HDMI_SURFACE_ABI)
		return -EINVAL;
	if (request->op != XG_HDMI_SURFACE_ALLOC &&
	    request->op != XG_HDMI_SURFACE_RELEASE &&
	    request->op != XG_HDMI_SURFACE_QUERY)
		return -EINVAL;
	mutex_lock(&xg_hdmi_surface_lock);
	if (request->op == XG_HDMI_SURFACE_RELEASE) {
		xg_hdmi_surface_free_locked();
		goto report;
	}
	if (request->op == XG_HDMI_SURFACE_QUERY) {
		if (!xg_hdmi_surface.obj)
			err = -ENOENT;
		goto report;
	}
	/* Only the Linux-verified CEA 1080p60 XRGB8888 first mode. */
	if (request->width != 1920u || request->height != 1080u ||
	    request->pitch != 7680u || request->bytes != bytes) {
		err = -EINVAL;
		goto report;
	}
	if (xg_hdmi_surface.obj) {
		err = -EBUSY;
		goto report;
	}
	if ((i915 = xg_i915_find(&pdev)) == NULL) {
		err = -ENODEV;
		goto report;
	}
	obj = i915_gem_object_create_shmem(i915, bytes);
	if (IS_ERR(obj)) {
		err = PTR_ERR(obj);
		goto put_pci;
	}
	/* Meteor Lake's display planes consume un-snooped memory.
	 * Keep the framebuffer in system memory and CPU-accessible.
	 */
	i915_gem_object_set_cache_coherency(obj, I915_CACHE_NONE);
	vma = i915_gem_object_ggtt_pin(obj, NULL, 0, 65536u, PIN_GLOBAL);
	if (IS_ERR(vma)) {
		err = PTR_ERR(vma);
		goto put_obj;
	}
	cpu = i915_gem_object_pin_map(obj, I915_MAP_WC);
	if (IS_ERR(cpu)) {
		err = PTR_ERR(cpu);
		goto unpin_vma;
	}
	memset(cpu, 0, bytes);
	i915_gem_object_flush_map(obj);
	if ((u64)i915_ggtt_offset(vma) + bytes > 0x100000000ULL) {
		err = -ERANGE; /* PLANE_SURF_B has a 32-bit GGTT offset */
		goto unpin_map;
	}
	xg_hdmi_surface.obj = obj;
	xg_hdmi_surface.vma = vma;
	xg_hdmi_surface.cpu = cpu;
	xg_hdmi_surface.offset = i915_ggtt_offset(vma);
	xg_hdmi_surface.size = bytes;
	pci_dev_put(pdev);
	goto report;
unpin_map:
	i915_gem_object_unpin_map(obj);
unpin_vma:
	i915_vma_unpin(vma);
put_obj:
	i915_gem_object_put(obj);
put_pci:
	pci_dev_put(pdev);
report:
	request->width = xg_hdmi_surface.obj ? 1920u : 0;
	request->height = xg_hdmi_surface.obj ? 1080u : 0;
	request->pitch = xg_hdmi_surface.obj ? 7680u : 0;
	request->bytes = xg_hdmi_surface.size;
	request->ggtt_offset = xg_hdmi_surface.offset;
	request->kva = (unsigned long long)(unsigned long)xg_hdmi_surface.cpu;
	mutex_unlock(&xg_hdmi_surface_lock);
	return err;
}
