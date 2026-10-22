/*
 * XeGfx: firmware files from the kext's Resources folder through OSKextRequestResource, made synchronous for
 * FreeBSD's firmware_get().
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/OSKextLib.h>
#include <libkern/OSAtomic.h>
#include <kern/clock.h>
#include <sys/errno.h>
#include <string.h>
#include "xg_misc.h"

#define XG_KEXT_ID	"org.watchthelight.XeGfx"

/* images linked into the binary (tools/link.sh, xg_fwblob.S); OSKextRequestResource failed for an AuxKC kext (N-228) */
struct xg_fw_blob {
	const char	*name;
	const void	*data;
	uint64_t	len;
};
extern "C" const xg_fw_blob xg_fw_embedded[];

/* shared by the waiter and the callback; whoever drops the last reference frees it, so a late callback after a
 * timeout never touches freed memory */
struct xg_fw_req {
	IOLock		*lock;
	volatile SInt32	refs;
	bool		done;
	OSReturn	result;
	void		*data;
	uint32_t	len;
};

static void
xg_fw_rele(xg_fw_req *r)
{
	if (OSDecrementAtomic(&r->refs) != 1)
		return;
	if (r->data != nullptr)
		IOFree(r->data, r->len);
	IOLockFree(r->lock);
	IOFree(r, sizeof(*r));
}

static void
xg_fw_done(OSKextRequestTag, OSReturn result, const void *data, uint32_t len, void *ctx)
{
	xg_fw_req *r = (xg_fw_req *)ctx;
	void *copy = nullptr;

	/* the daemon's buffer is gone once this returns */
	if (result == kOSReturnSuccess && data != nullptr && len != 0) {
		copy = IOMalloc(len);
		if (copy != nullptr)
			memcpy(copy, data, len);
		else
			result = kOSReturnError;
	} else if (result == kOSReturnSuccess)
		result = kOSKextReturnNotFound;
	IOLockLock(r->lock);
	r->result = result;
	r->data = copy;
	r->len = copy != nullptr ? len : 0;
	r->done = true;
	IOLockWakeup(r->lock, r, false);
	IOLockUnlock(r->lock);
	xg_fw_rele(r);
}

extern "C" int
xg_misc_fw_request(const char *name, void **datap, unsigned long *lenp, unsigned int timeout_ms)
{
	OSKextRequestTag tag;
	AbsoluteTime deadline;
	xg_fw_req *r;
	void *ctx;
	int error;

	*datap = nullptr;
	*lenp = 0;
	for (const xg_fw_blob *b = xg_fw_embedded; b->name != nullptr; b++) {
		if (strcmp(b->name, name) != 0)
			continue;
		if ((*datap = IOMalloc(b->len)) == nullptr)
			return (ENOMEM);
		memcpy(*datap, b->data, b->len);
		*lenp = b->len;
		return (0);
	}
	r = (xg_fw_req *)IOMallocZero(sizeof(*r));
	if (r == nullptr)
		return (ENOMEM);
	r->lock = IOLockAlloc();
	if (r->lock == nullptr) {
		IOFree(r, sizeof(*r));
		return (ENOMEM);
	}
	r->refs = 2;
	if (OSKextRequestResource(XG_KEXT_ID, name, xg_fw_done, r, &tag) != kOSReturnSuccess) {
		r->refs = 1;
		xg_fw_rele(r);
		return (ENOENT);
	}
	clock_interval_to_deadline(timeout_ms, kMillisecondScale, &deadline);
	IOLockLock(r->lock);
	while (!r->done)
		if (IOLockSleepDeadline(r->lock, r, deadline, THREAD_UNINT) == THREAD_TIMED_OUT)
			break;
	if (!r->done) {
		IOLockUnlock(r->lock);
		/* a cancelled request never calls back, so its reference is ours to drop */
		if (OSKextCancelRequest(tag, &ctx) == kOSReturnSuccess)
			xg_fw_rele(r);
		xg_fw_rele(r);
		return (ETIMEDOUT);
	}
	if (r->result == kOSReturnSuccess) {
		*datap = r->data;
		*lenp = r->len;
		r->data = nullptr;
		error = 0;
	} else
		error = r->result == kOSKextReturnTimeout ? ETIMEDOUT : ENOENT;
	IOLockUnlock(r->lock);
	xg_fw_rele(r);
	return (error);
}
