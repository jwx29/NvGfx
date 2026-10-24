/*
 * XeGfx: the XNU side of the FreeBSD cdevs in xg_fd.c, one BSD cdevsw whose devfs clone nodes give every open its
 * own minor, so each open has its own FreeBSD struct file (and so its own drm_file).
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <sys/types.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/conf.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <sys/ioccom.h>
#include <sys/select.h>
#include <sys/kauth.h>
#include <sys/vnode.h>
#include <miscfs/devfs/devfs.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/OSAtomic.h>
#include "xg_fd.h"

/* XNU's struct selinfo is opaque to kexts; a zeroed one is valid, selrecord sets it up on first use */
#define XG_FD_SELINFO_SIZE 128
struct xg_fd_sel {
	char	si[XG_FD_SELINFO_SIZE] __attribute__((aligned(16)));
	bool	recorded;
};
static xg_fd_sel xg_fd_sels[XG_FD_NINST];
static IOLock *xg_fd_sel_lock;

enum { XS_NONE, XS_BUSY, XS_READY, XS_FAILED };
static volatile UInt32 xg_fd_state;
static int xg_fd_major = -1;

/* minor() and makedev() from sys/types.h, without their sign conversion warnings */
static int
xg_fd_minor(dev_t dev)
{
	return ((int)((uint32_t)dev & 0xffffff));
}

static dev_t
xg_fd_makedev(int major, int minor)
{
	return ((dev_t)(((uint32_t)major << 24) | ((uint32_t)minor & 0xffffff)));
}

static int
xg_fd_inst_of(dev_t dev)
{
	int m = xg_fd_minor(dev);

	if ((m & XG_FD_INST_FLAG) == 0 || XG_FD_MINOR_INST(m) >= XG_FD_NINST)
		return (-1);
	return (XG_FD_MINOR_INST(m));
}

extern "C" int xg_printf(const char *, ...) __attribute__((__format__(__printf__, 1, 2)));

/* I-113: ANV turns a failed execbuffer into VK_ERROR_DEVICE_LOST and reports the errno only on WindowServer's
 * stderr; put failing ioctls in the ring instead (every execbuffer failure, the first 64 others per process, so the
 * apps' GETPARAM probes cannot use up WindowServer's lines as they did in N-280) */
#define XG_NOTE_PIDS 16
static volatile SInt32 xg_note_pid[XG_NOTE_PIDS], xg_note_cnt[XG_NOTE_PIDS];

static bool
xg_fd_note_budget(int pid)
{
	for (int i = 0; i < XG_NOTE_PIDS; i++) {
		if (xg_note_pid[i] == 0)
			OSCompareAndSwap(0, (UInt32)pid, (volatile UInt32 *)&xg_note_pid[i]);
		if (xg_note_pid[i] == pid)
			return (OSIncrementAtomic(&xg_note_cnt[i]) < 64);
	}
	return (false);
}

static int
xg_fd_ioctl_note(int inst, unsigned long cmd, int error)
{
	if (error == 0 || error == EINTR || error == EAGAIN || error == ETIME || error == ERESTART)
		return (error);
	if ((cmd & 0xff) == 0x69 || xg_fd_note_budget(proc_selfpid()))
		xg_printf("xegfx: ioctl 0x%lx (nr 0x%lx) inst %d pid %d -> errno %d\n", cmd, cmd & 0xff, inst,
		    proc_selfpid(), error);
	return (error);
}

/* the FreeBSD errnos that XNU numbers differently (sys/errno.h of both); the rest match */
static int
xg_fd_errno(int e)
{
	switch (e) {
	case 82: return (EIDRM);
	case 83: return (ENOMSG);
	case 85: return (ECANCELED);
	case 86: return (EILSEQ);
	case 87: return (ENOATTR);
	case 88: return (EINVAL);	/* EDOOFUS */
	case 89: return (EBADMSG);
	case 90: return (EMULTIHOP);
	case 91: return (ENOLINK);
	case 92: return (EPROTO);
	case 93: return (EPERM);	/* ENOTCAPABLE */
	case 94: return (EPERM);	/* ECAPMODE */
	case 95: return (ENOTRECOVERABLE);
	case 96: return (EOWNERDEAD);
	case 97: return (EIO);		/* EINTEGRITY */
	default: return (e);
	}
}

/* LinuxKPI makes ETIME ETIMEDOUT (60); libdrm and Mesa on Darwin test for ETIME (101), so a timed-out syncobj or
 * GEM wait read as a failure (VK_ERROR_UNKNOWN) instead of VK_TIMEOUT */
static int
xg_fd_ioctl_errno(int e)
{
	return (e == ETIMEDOUT ? ETIME : xg_fd_errno(e));
}

static int
xg_fd_d_open(dev_t dev, int flags, int devtype, struct proc *p)
{
	int inst = xg_fd_inst_of(dev);

	if (inst < 0)
		return (ENXIO);
	return (xg_fd_errno(xg_fd_dev_open(inst, flags)));
}

static int
xg_fd_d_close(dev_t dev, int flags, int devtype, struct proc *p)
{
	int inst = xg_fd_inst_of(dev);

	if (inst < 0)
		return (0);
	/* before the instance can be reused */
	IOLockLock(xg_fd_sel_lock);
	if (xg_fd_sels[inst].recorded)
		selthreadclear((struct selinfo *)xg_fd_sels[inst].si);
	bzero(&xg_fd_sels[inst], sizeof(xg_fd_sels[inst]));
	IOLockUnlock(xg_fd_sel_lock);
	return (xg_fd_errno(xg_fd_dev_close(inst)));
}

static int
xg_fd_d_rw(dev_t dev, struct uio *uio, int ioflag, int write)
{
	int inst = xg_fd_inst_of(dev), error;
	unsigned long done = 0;
	long long off;
	user_size_t len;

	if (inst < 0)
		return (ENXIO);
	if (!uio_isuserspace(uio))
		return (EINVAL);
	/* LinuxKPI takes one iovec per call */
	len = uio_curriovlen(uio);
	if (len == 0)
		return (0);
	off = uio_offset(uio);
	error = xg_fd_dev_rw(inst, write, uio_curriovbase(uio), len, &off, (ioflag & IO_NDELAY) != 0, &done);
	if (done != 0)
		uio_update(uio, done);
	return (xg_fd_errno(error));
}

static int
xg_fd_d_read(dev_t dev, struct uio *uio, int ioflag)
{
	return (xg_fd_d_rw(dev, uio, ioflag, 0));
}

static int
xg_fd_d_write(dev_t dev, struct uio *uio, int ioflag)
{
	return (xg_fd_d_rw(dev, uio, ioflag, 1));
}

static int
xg_fd_d_ioctl(dev_t dev, u_long cmd, caddr_t data, int fflag, struct proc *p)
{
	int inst = xg_fd_inst_of(dev);

	if (inst < 0)
		return (ENXIO);
	return (xg_fd_ioctl_note(inst, cmd, xg_fd_ioctl_errno(xg_fd_dev_ioctl(inst, cmd, data, fflag))));
}

/* what d_ioctl would get, with the copies ioctl(2) makes around it; only the opener may use an instance */
extern "C" int
xg_fd_remote_ioctl(int minor, unsigned long cmd, unsigned long long uarg)
{
	int inst = xg_fd_inst_of(xg_fd_makedev(0, minor)), error;
	size_t len = IOCPARM_LEN(cmd);
	char small[256] __attribute__((aligned(16)));
	char *buf = small;

	if (inst < 0 || xg_fd_inst_owner(inst) != proc_selfpid())
		return (EBADF);
	if (len > IOCPARM_MAX)
		return (ENOTTY);
	if (len > sizeof(small) && (buf = (char *)IOMalloc(len)) == NULL)
		return (ENOMEM);
	bzero(buf, len > sizeof(small) ? len : sizeof(small));
	error = 0;
	if ((cmd & IOC_IN) && len != 0)
		error = copyin((user_addr_t)uarg, buf, len);
	else if ((cmd & IOC_VOID) && len == 0)
		*(user_addr_t *)buf = (user_addr_t)uarg;
	if (error == 0)
		error = xg_fd_ioctl_note(inst, cmd, xg_fd_ioctl_errno(xg_fd_dev_ioctl(inst, cmd, buf, 0)));
	if (error == 0 && (cmd & IOC_OUT) && len != 0)
		error = copyout(buf, (user_addr_t)uarg, len);
	if (buf != small)
		IOFree(buf, len);
	return (error);
}

static int
xg_fd_d_select(dev_t dev, int which, void *wql, struct proc *p)
{
	int inst = xg_fd_inst_of(dev);

	if (inst < 0)
		return (1);
	/* record first, then poll: a wakeup between the two is not lost */
	IOLockLock(xg_fd_sel_lock);
	selrecord(p, (struct selinfo *)xg_fd_sels[inst].si, wql);
	xg_fd_sels[inst].recorded = true;
	IOLockUnlock(xg_fd_sel_lock);
	return (xg_fd_dev_poll(inst, which));
}

static struct cdevsw xg_fd_cdevsw = {
	xg_fd_d_open, xg_fd_d_close, xg_fd_d_read, xg_fd_d_write, xg_fd_d_ioctl, eno_stop, eno_reset, NULL,
	xg_fd_d_select, eno_mmap, eno_strat, eno_getc, eno_putc, 0
};

/* devfs calls this with its own lock held: ALLOC reserves an instance for the open that follows */
static int
xg_fd_clone(dev_t dev, int action)
{
	int m = xg_fd_minor(dev), inst;

	if (action == DEVFS_CLONE_ALLOC) {
		if ((m & XG_FD_INST_FLAG) != 0)
			return (-1);
		inst = xg_fd_inst_alloc(m);
		return (inst < 0 ? -1 : XG_FD_INST_MINOR(inst, m));
	}
	if ((m & XG_FD_INST_FLAG) != 0)
		xg_fd_inst_free(XG_FD_MINOR_INST(m));
	return (0);
}

/* one cdevsw for every XeGfx node, registered on the first make_dev */
static bool
xg_fd_register(void)
{
	for (;;) {
		UInt32 s = xg_fd_state;

		if (s == XS_READY)
			return (true);
		if (s == XS_FAILED)
			return (false);
		if (s == XS_BUSY) {
			IOSleep(1);
			continue;
		}
		if (!OSCompareAndSwap(XS_NONE, XS_BUSY, &xg_fd_state))
			continue;
		xg_fd_sel_lock = IOLockAlloc();
		/* -24: search for a free major from 24 up, as Apple's sample drivers do */
		xg_fd_major = xg_fd_sel_lock != NULL ? cdevsw_add(-24, &xg_fd_cdevsw) : -1;
		if (xg_fd_major < 0) {
			IOLog("XeGfx: no free cdevsw major\n");
			if (xg_fd_sel_lock != NULL)
				IOLockFree(xg_fd_sel_lock);
			xg_fd_sel_lock = NULL;
			OSCompareAndSwap(XS_BUSY, XS_FAILED, &xg_fd_state);
			return (false);
		}
		OSCompareAndSwap(XS_BUSY, XS_READY, &xg_fd_state);
		return (true);
	}
}

extern "C" int
xg_fd_xnu_node_create(int slot, unsigned int uid, unsigned int gid, int mode, const char *name, void **handlep)
{
	void *h;

	if (!xg_fd_register())
		return (ENXIO);
	h = devfs_make_node_clone(xg_fd_makedev(xg_fd_major, slot), DEVFS_CHAR, uid, gid, mode & 07777, xg_fd_clone,
	    "%s", name);
	if (h == NULL)
		return (EEXIST);
	*handlep = h;
	return (0);
}

extern "C" void
xg_fd_xnu_node_remove(void *handle)
{
	devfs_remove(handle);
}

extern "C" unsigned int
xg_fd_xnu_udev(int slot)
{
	return (xg_fd_major < 0 ? 0xffffffffu : (unsigned int)xg_fd_makedev(xg_fd_major, slot));
}

extern "C" int
xg_fd_xnu_pid(void)
{
	return (proc_selfpid());
}

extern "C" int
xg_fd_xnu_issuser(void)
{
	return (kauth_cred_issuser(kauth_cred_get()));
}

extern "C" void
xg_fd_xnu_selwakeup_all(void)
{
	if (xg_fd_sel_lock == NULL)
		return;
	IOLockLock(xg_fd_sel_lock);
	for (int i = 0; i < XG_FD_NINST; i++)
		if (xg_fd_sels[i].recorded)
			selwakeup((struct selinfo *)xg_fd_sels[i].si);
	IOLockUnlock(xg_fd_sel_lock);
}

/* at unload, after every node is destroyed */
extern "C" void
xg_fd_xnu_fini(void)
{
	if (xg_fd_state != XS_READY)
		return;
	cdevsw_remove(xg_fd_major, &xg_fd_cdevsw);
	xg_fd_major = -1;
	IOLockFree(xg_fd_sel_lock);
	xg_fd_sel_lock = NULL;
	xg_fd_state = XS_NONE;
}
