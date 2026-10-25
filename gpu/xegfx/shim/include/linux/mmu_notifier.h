/*
 * XeGfx: the mmu interval notifier API i915's userptr objects use (X5), in place of LinuxKPI's empty mmu_notifier.h.
 * XNU gives a kext no hook when a process unmaps or remaps memory, so nothing ever invalidates: a userptr's pages are
 * wired by vm_fault_quick_hold_pages for as long as i915 holds them, so the GPU never sees freed memory, only data the
 * process may have replaced after it unmapped the range (its own bug, as with any wired DMA buffer).
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#ifndef _XG_LINUX_MMU_NOTIFIER_H_
#define	_XG_LINUX_MMU_NOTIFIER_H_

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/mm_types.h>

struct mm_struct;

struct mmu_notifier {
};

struct mmu_notifier_range {
	unsigned long start;
	unsigned long end;
};

struct mmu_interval_notifier;

struct mmu_interval_notifier_ops {
	bool (*invalidate)(struct mmu_interval_notifier *mni, const struct mmu_notifier_range *range,
	    unsigned long cur_seq);
};

struct mmu_interval_notifier {
	struct mm_struct *mm;
	const struct mmu_interval_notifier_ops *ops;
	unsigned long start;
	unsigned long length;
	unsigned long invalidate_seq;
};

static inline int
mmu_interval_notifier_insert(struct mmu_interval_notifier *mni, struct mm_struct *mm, unsigned long start,
    unsigned long length, const struct mmu_interval_notifier_ops *ops)
{
	/* Linux holds the mm until remove; i915 keeps comparing notifier.mm with current->mm after the task exits */
	if (mm != NULL)
		mmgrab(mm);
	mni->mm = mm;
	mni->ops = ops;
	mni->start = start;
	mni->length = length;
	WRITE_ONCE(mni->invalidate_seq, 2);	/* even: no invalidation running */
	return (0);
}

static inline void
mmu_interval_notifier_remove(struct mmu_interval_notifier *mni)
{
	mni->ops = NULL;
	if (mni->mm != NULL)
		mmdrop(mni->mm);
}

static inline unsigned long
mmu_interval_read_begin(struct mmu_interval_notifier *mni)
{
	return (READ_ONCE(mni->invalidate_seq));
}

static inline bool
mmu_interval_read_retry(struct mmu_interval_notifier *mni, unsigned long seq)
{
	return (READ_ONCE(mni->invalidate_seq) != seq);
}

static inline void
mmu_interval_set_seq(struct mmu_interval_notifier *mni, unsigned long cur_seq)
{
	WRITE_ONCE(mni->invalidate_seq, cur_seq);
}

#endif
