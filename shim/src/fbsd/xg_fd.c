/*
 * XeGfx: FreeBSD character devices and file descriptors on XNU, the part LinuxKPI needs for the DRM nodes.
 * make_dev*() names a cdev and asks xg_fd.cpp for an XNU devfs clone node; every open of that node gets its own
 * instance and its own FreeBSD struct file, opened through the cdevsw's d_fdopen as devfs would. Descriptors that
 * LinuxKPI hands out (dma-buf, sync_file) live in a private per-process table, see docs/xg_fd.md.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/ctype.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/ucred.h>
#include <sys/capsicum.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/conf.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/poll.h>
#include <sys/selinfo.h>
#include <sys/priv.h>
#include <sys/resource.h>
#include <sys/resourcevar.h>
#include <sys/sigio.h>
#include <sys/eventfd.h>
#include <sys/filio.h>
#include <sys/refcount.h>
#include <machine/atomic.h>
#include <machine/stdarg.h>
#include "xg_fd.h"

static MALLOC_DEFINE(M_XGFD, "xgfd", "XeGfx files and device nodes");

const cap_rights_t cap_no_rights = CAP_RIGHTS_INITIALIZER(0ULL);

#define XG_FD_MAXFILES	65536	/* per private table, also what lim_cur_proc reports for RLIMIT_NOFILE */
#define XG_FD_MAXOPEN	(1 << 20)

/*
 * One lock for cdev slots, instances and descriptor tables. Never held across a call into a cdevsw, a fileops
 * method or XNU devfs: the devfs clone callback takes it with the devfs lock held.
 */
static struct mtx xg_fd_mtx;
static volatile u_int xg_fd_mtx_state;
static volatile u_int xg_fd_openfiles;

static void
xg_fd_lock(void)
{
	/* lazy init: make_dev can run before any SYSINIT */
	if (__predict_false(atomic_load_acq_int(&xg_fd_mtx_state) != 2)) {
		if (atomic_cmpset_int(&xg_fd_mtx_state, 0, 1)) {
			mtx_init(&xg_fd_mtx, "xgfd", NULL, MTX_DEF);
			atomic_store_rel_int(&xg_fd_mtx_state, 2);
		} else {
			while (atomic_load_acq_int(&xg_fd_mtx_state) != 2)
				cpu_spinwait();
		}
	}
	mtx_lock(&xg_fd_mtx);
}

static void
xg_fd_unlock(void)
{
	mtx_unlock(&xg_fd_mtx);
}

/* badfileops: a file between falloc and finit */
static int
xg_fd_badfo_rdwr(struct file *fp, struct uio *uio, struct ucred *cred, int flags, struct thread *td)
{
	return (EBADF);
}

static int
xg_fd_badfo_truncate(struct file *fp, off_t length, struct ucred *cred, struct thread *td)
{
	return (EINVAL);
}

static int
xg_fd_badfo_ioctl(struct file *fp, u_long com, void *data, struct ucred *cred, struct thread *td)
{
	return (EBADF);
}

static int
xg_fd_badfo_poll(struct file *fp, int events, struct ucred *cred, struct thread *td)
{
	return (0);
}

static int
xg_fd_badfo_kqfilter(struct file *fp, struct knote *kn)
{
	return (EBADF);
}

static int
xg_fd_badfo_stat(struct file *fp, struct stat *sb, struct ucred *cred)
{
	return (EBADF);
}

static int
xg_fd_badfo_close(struct file *fp, struct thread *td)
{
	return (0);
}

static int
xg_fd_badfo_chmod(struct file *fp, mode_t mode, struct ucred *cred, struct thread *td)
{
	return (EBADF);
}

static int
xg_fd_badfo_chown(struct file *fp, uid_t uid, gid_t gid, struct ucred *cred, struct thread *td)
{
	return (EBADF);
}

static int
xg_fd_badfo_sendfile(struct file *fp, int sockfd, struct uio *hdr_uio, struct uio *trl_uio, off_t offset,
    size_t nbytes, off_t *sent, int flags, struct thread *td)
{
	return (EBADF);
}

static const struct fileops xg_fd_badfileops = {
	.fo_read = xg_fd_badfo_rdwr,
	.fo_write = xg_fd_badfo_rdwr,
	.fo_truncate = xg_fd_badfo_truncate,
	.fo_ioctl = xg_fd_badfo_ioctl,
	.fo_poll = xg_fd_badfo_poll,
	.fo_kqfilter = xg_fd_badfo_kqfilter,
	.fo_stat = xg_fd_badfo_stat,
	.fo_close = xg_fd_badfo_close,
	.fo_chmod = xg_fd_badfo_chmod,
	.fo_chown = xg_fd_badfo_chown,
	.fo_sendfile = xg_fd_badfo_sendfile,
};

/* the invfo_* defaults from kern_descrip.c */
int
invfo_rdwr(struct file *fp, struct uio *uio, struct ucred *active_cred, int flags, struct thread *td)
{
	return (EOPNOTSUPP);
}

int
invfo_truncate(struct file *fp, off_t length, struct ucred *active_cred, struct thread *td)
{
	return (EINVAL);
}

int
invfo_kqfilter(struct file *fp, struct knote *kn)
{
	return (EINVAL);
}

int
invfo_chmod(struct file *fp, mode_t mode, struct ucred *active_cred, struct thread *td)
{
	return (EINVAL);
}

int
invfo_chown(struct file *fp, uid_t uid, gid_t gid, struct ucred *active_cred, struct thread *td)
{
	return (EINVAL);
}

int
invfo_sendfile(struct file *fp, int sockfd, struct uio *hdr_uio, struct uio *trl_uio, off_t offset,
    size_t nbytes, off_t *sent, int flags, struct thread *td)
{
	return (EINVAL);
}

/* struct file */
int
_falloc_noinstall(struct thread *td, struct file **resultfp, u_int n)
{
	struct file *fp;

	MPASS(n > 0);
	if (atomic_fetchadd_int(&xg_fd_openfiles, 1) + 1 > XG_FD_MAXOPEN) {
		atomic_subtract_int(&xg_fd_openfiles, 1);
		return (ENFILE);
	}
	fp = malloc(sizeof(*fp), M_XGFD, M_WAITOK | M_ZERO);
	if (fp == NULL) {
		atomic_subtract_int(&xg_fd_openfiles, 1);
		return (ENOMEM);
	}
	refcount_init(&fp->f_count, n);
	/* shim credentials are static (xg_kern.c), so no crhold */
	fp->f_cred = td->td_ucred;
	fp->f_ops = &xg_fd_badfileops;
	*resultfp = fp;
	return (0);
}

void
finit(struct file *fp, u_int flag, short type, void *data, const struct fileops *ops)
{
	fp->f_data = data;
	fp->f_flag = flag;
	fp->f_type = type;
	atomic_store_rel_ptr((volatile uintptr_t *)&fp->f_ops, (uintptr_t)ops);
}

int
_fdrop(struct file *fp, struct thread *td)
{
	int error;

	KASSERT(refcount_load(&fp->f_count) == 0, ("fdrop: fp %p count %d", fp, refcount_load(&fp->f_count)));
	error = fo_close(fp, td);
	atomic_subtract_int(&xg_fd_openfiles, 1);
	free(fp, M_XGFD);
	return (error);
}

/*
 * Private descriptor tables, one per XNU pid. XNU gives kexts no way to install a file in a process's own table, so
 * the numbers handed out here mean something only to XeGfx ioctls of the same process. A table is closed when the
 * last device instance its process opened is closed.
 */
struct xg_fd_tbl {
	LIST_ENTRY(xg_fd_tbl) ft_link;
	int		ft_pid;
	int		ft_devrefs;
	int		ft_nfiles;
	struct file	**ft_files;
};
static LIST_HEAD(, xg_fd_tbl) xg_fd_tbls = LIST_HEAD_INITIALIZER(xg_fd_tbls);

static struct xg_fd_tbl *
xg_fd_tbl_find(int pid)
{
	struct xg_fd_tbl *t;

	mtx_assert(&xg_fd_mtx, MA_OWNED);
	LIST_FOREACH(t, &xg_fd_tbls, ft_link)
		if (t->ft_pid == pid)
			return (t);
	return (NULL);
}

static struct file *
xg_fd_tbl_get(int fd)
{
	struct xg_fd_tbl *t;
	u_int i;

	if (fd < XG_FD_BASE)
		return (NULL);
	i = (u_int)(fd - XG_FD_BASE);
	t = xg_fd_tbl_find(xg_fd_xnu_pid());
	if (t == NULL || i >= (u_int)t->ft_nfiles)
		return (NULL);
	return (t->ft_files[i]);
}

/* drops every file of a table taken off the list */
static void
xg_fd_tbl_close(struct xg_fd_tbl *t, struct thread *td)
{
	for (int i = 0; i < t->ft_nfiles; i++)
		if (t->ft_files[i] != NULL)
			fdrop(t->ft_files[i], td);
	free(t->ft_files, M_XGFD);
	free(t, M_XGFD);
}

/* puts fp (whose table reference the caller already counted) in the lowest free slot */
static int
xg_fd_tbl_install(struct file *fp, int *fdp)
{
	struct xg_fd_tbl *t, *nt = NULL;
	struct file **nf = NULL, **old;
	int pid = xg_fd_xnu_pid(), n = 0, error = 0;

	for (;;) {
		xg_fd_lock();
		t = xg_fd_tbl_find(pid);
		if (t == NULL && nt != NULL) {
			t = nt;
			nt = NULL;
			t->ft_pid = pid;
			LIST_INSERT_HEAD(&xg_fd_tbls, t, ft_link);
		}
		if (t != NULL && nf != NULL && n > t->ft_nfiles) {
			/* the table may have grown while unlocked; only a larger array replaces it */
			if (t->ft_nfiles > 0)
				memcpy(nf, t->ft_files, t->ft_nfiles * sizeof(*nf));
			old = t->ft_files;
			t->ft_files = nf;
			t->ft_nfiles = n;
			nf = old;
		}
		if (t != NULL) {
			for (int i = 0; i < t->ft_nfiles; i++) {
				if (t->ft_files[i] == NULL) {
					t->ft_files[i] = fp;
					*fdp = XG_FD_BASE + i;
					xg_fd_unlock();
					goto out;
				}
			}
			n = t->ft_nfiles != 0 ? t->ft_nfiles * 2 : 16;
		}
		xg_fd_unlock();
		free(nf, M_XGFD);
		nf = NULL;
		if (t == NULL) {
			nt = malloc(sizeof(*nt), M_XGFD, M_WAITOK | M_ZERO);
			if (nt == NULL)
				return (ENOMEM);
			continue;
		}
		if (n > XG_FD_MAXFILES) {
			error = EMFILE;
			goto out;
		}
		nf = malloc(n * sizeof(*nf), M_XGFD, M_WAITOK | M_ZERO);
		if (nf == NULL) {
			error = ENOMEM;
			goto out;
		}
	}
out:
	free(nf, M_XGFD);
	free(nt, M_XGFD);
	return (error);
}

int
finstall(struct thread *td, struct file *fp, int *resultfd, int flags, struct filecaps *fcaps)
{
	int error;

	MPASS(resultfd != NULL);
	if (!fhold(fp))
		return (EBADF);
	/* flags (O_CLOEXEC) and fcaps mean nothing for a private table */
	error = xg_fd_tbl_install(fp, resultfd);
	if (error != 0)
		fdrop(fp, td);
	return (error);
}

int
falloc_caps(struct thread *td, struct file **resultfp, int *resultfd, int flags, struct filecaps *fcaps)
{
	struct file *fp;
	int error, fd;

	MPASS(resultfp != NULL);
	MPASS(resultfd != NULL);
	error = _falloc_noinstall(td, &fp, 2);
	if (error != 0)
		return (error);
	error = xg_fd_tbl_install(fp, &fd);
	if (error != 0) {
		/* falloc_abort: nobody else saw it */
		refcount_init(&fp->f_count, 0);
		_fdrop(fp, td);
		return (error);
	}
	*resultfp = fp;
	*resultfd = fd;
	return (0);
}

void
fdclose(struct thread *td, struct file *fp, int idx)
{
	struct xg_fd_tbl *t;
	u_int i = (u_int)(idx - XG_FD_BASE);

	xg_fd_lock();
	t = xg_fd_tbl_find(xg_fd_xnu_pid());
	if (idx >= XG_FD_BASE && t != NULL && i < (u_int)t->ft_nfiles && t->ft_files[i] == fp) {
		t->ft_files[i] = NULL;
		xg_fd_unlock();
		fdrop(fp, td);
	} else
		xg_fd_unlock();
}

int
fget_unlocked(struct thread *td, int fd, const cap_rights_t *needrightsp, struct file **fpp)
{
	struct file *fp;

	/* no capability mode on XNU: rights are not checked */
	xg_fd_lock();
	fp = xg_fd_tbl_get(fd);
	if (fp == NULL || !fhold(fp)) {
		xg_fd_unlock();
		return (EBADF);
	}
	xg_fd_unlock();
	*fpp = fp;
	return (0);
}

int
fget(struct thread *td, int fd, const cap_rights_t *rightsp, struct file **fpp)
{
	struct file *fp;
	int error;

	error = fget_unlocked(td, fd, rightsp, &fp);
	if (error != 0)
		return (error);
	/* like _fget: a reserved but not yet finit'ed slot is not a file */
	if (fp->f_ops == &xg_fd_badfileops) {
		fdrop(fp, td);
		return (EBADF);
	}
	*fpp = fp;
	return (0);
}

pid_t
fgetown(struct sigio **sigiop)
{
	struct sigio *sio;

	/* sigio_lock belongs to fsetown's area; one atomic read is enough for a pid */
	sio = (struct sigio *)atomic_load_acq_ptr((volatile uintptr_t *)sigiop);
	return (sio != NULL ? sio->sio_pgid : 0);
}

int
priv_check(struct thread *td, int priv)
{
	/* every privilege is the superuser's; XNU decides who that is */
	return (xg_fd_xnu_issuser() ? 0 : EPERM);
}

rlim_t
lim_cur_proc(struct proc *p, int which)
{
	return (which == RLIMIT_NOFILE ? XG_FD_MAXFILES : RLIM_INFINITY);
}

/*
 * Character devices. Each named cdev, alias or not, is an xg_fd_cdev; the embedded vnode is what LinuxKPI sees as
 * the inode (v_rdev for iminor, vref/vrele per open file).
 */
struct xg_fd_cdev {
	struct cdev	xc_cdev;	/* first, so a struct cdev pointer is one */
	struct vnode	xc_vnode;
	void		*xc_node;	/* XNU devfs handle */
	int		xc_slot;	/* XNU minor of the node; an alias uses its parent's */
	u_int		xc_refs;	/* creation + open instances */
	u_int		xc_busy;	/* opens in the cdevsw right now */
	bool		xc_gone;
	LIST_ENTRY(xg_fd_cdev) xc_all;
};
static struct xg_fd_cdev *xg_fd_slots[XG_FD_NCDEV];
static LIST_HEAD(, xg_fd_cdev) xg_fd_names = LIST_HEAD_INITIALIZER(xg_fd_names);

static void
xg_fd_cdev_free(struct xg_fd_cdev *xc)
{
	/* an open file can outlive the cdev and still vrele this vnode: then keep the memory */
	if (xc->xc_vnode.v_usecount > 1) {
		printf("xegfx: %s freed with %d vnode refs, leaking it\n", xc->xc_cdev.si_name,
		    (int)xc->xc_vnode.v_usecount);
		return;
	}
	free(xc, M_XGFD);
}

static void
xg_fd_cdev_rele(struct xg_fd_cdev *xc)
{
	bool last;

	xg_fd_lock();
	last = --xc->xc_refs == 0;
	xg_fd_unlock();
	if (last)
		xg_fd_cdev_free(xc);
}

/* prep_devname from kern_conf.c, with the duplicate check against XeGfx's own names */
static int
xg_fd_prep_name(struct cdev *dev, const char *fmt, va_list ap)
{
	struct xg_fd_cdev *xc;
	char *from, *q, *s, *to;
	int len;

	mtx_assert(&xg_fd_mtx, MA_OWNED);
	len = vsnprintf(dev->si_name, sizeof(dev->si_name), fmt, ap);
	if (len > (int)sizeof(dev->si_name) - 1)
		return (ENAMETOOLONG);
	for (from = dev->si_name; *from == '/'; from++)
		;
	for (to = dev->si_name; *from != '\0'; from++, to++) {
		if (isspace(*from) || *from == '"')
			return (EINVAL);
		while (from[0] == '/' && from[1] == '/')
			from++;
		if (from[0] == '/' && from[1] == '\0')
			return (EINVAL);
		*to = *from;
	}
	*to = '\0';
	if (dev->si_name[0] == '\0')
		return (EINVAL);
	for (s = dev->si_name;;) {
		for (q = s; *q != '/' && *q != '\0'; q++)
			;
		if (q - s == 1 && s[0] == '.')
			return (EINVAL);
		if (q - s == 2 && s[0] == '.' && s[1] == '.')
			return (EINVAL);
		if (*q != '/')
			break;
		s = q + 1;
	}
	LIST_FOREACH(xc, &xg_fd_names, xc_all)
		if (strcmp(xc->xc_cdev.si_name, dev->si_name) == 0)
			return (EEXIST);
	return (0);
}

/* allocates a named xg_fd_cdev, a slot too unless it is an alias */
static int
xg_fd_cdev_new(int flags, bool alias, struct xg_fd_cdev **xcp, const char *fmt, va_list ap)
{
	struct xg_fd_cdev *xc;
	int error, slot = -1;

	xc = malloc(sizeof(*xc), M_XGFD, M_ZERO | ((flags & MAKEDEV_NOWAIT) != 0 ? M_NOWAIT : M_WAITOK));
	if (xc == NULL)
		return (ENOMEM);
	xg_fd_lock();
	error = xg_fd_prep_name(&xc->xc_cdev, fmt, ap);
	if (error == 0 && !alias) {
		for (slot = 0; slot < XG_FD_NCDEV && xg_fd_slots[slot] != NULL; slot++)
			;
		if (slot == XG_FD_NCDEV)
			error = ENOSPC;
		else
			xg_fd_slots[slot] = xc;
	}
	if (error == 0)
		LIST_INSERT_HEAD(&xg_fd_names, xc, xc_all);
	xg_fd_unlock();
	if (error != 0) {
		/* FreeBSD panics here without MAKEDEV_CHECKNAME; an error is kinder to a running Mac */
		printf("xegfx: make_dev %s: error %d\n", xc->xc_cdev.si_name, error);
		free(xc, M_XGFD);
		return (error);
	}
	xc->xc_slot = slot;
	xc->xc_refs = 1;
	LIST_INIT(&xc->xc_cdev.si_children);
	xc->xc_vnode.v_type = VCHR;
	xc->xc_vnode.v_rdev = &xc->xc_cdev;
	xc->xc_vnode.v_usecount = 1;	/* the cdev's own */
	xc->xc_vnode.v_holdcnt = 1;
	*xcp = xc;
	return (0);
}

/* undoes xg_fd_cdev_new after the XNU node could not be made */
static void
xg_fd_cdev_unname(struct xg_fd_cdev *xc)
{
	xg_fd_lock();
	LIST_REMOVE(xc, xc_all);
	if ((xc->xc_cdev.si_flags & SI_ALIAS) == 0)
		xg_fd_slots[xc->xc_slot] = NULL;
	xg_fd_unlock();
	free(xc, M_XGFD);
}

void
make_dev_args_init_impl(struct make_dev_args *args, size_t sz)
{
	bzero(args, sz);
	args->mda_size = sz;
}

static int
xg_fd_make_dev_sv(struct make_dev_args *args1, struct cdev **dres, const char *fmt, va_list ap)
{
	struct make_dev_args args;
	struct xg_fd_cdev *xc;
	struct cdev *dev;
	int error;

	bzero(&args, sizeof(args));
	if (sizeof(args) < args1->mda_size)
		return (EINVAL);
	bcopy(args1, &args, args1->mda_size);
	if (args.mda_devsw == NULL || args.mda_devsw->d_version != D_VERSION) {
		printf("xegfx: make_dev: cdevsw %s has the wrong d_version\n",
		    args.mda_devsw != NULL ? args.mda_devsw->d_name : "(null)");
		return (EINVAL);
	}
	error = xg_fd_cdev_new(args.mda_flags, false, &xc, fmt, ap);
	if (error != 0)
		return (error);
	dev = &xc->xc_cdev;
	dev->si_flags = SI_NAMED | ((args.mda_flags & MAKEDEV_ETERNAL) != 0 ? SI_ETERNAL : 0);
	dev->si_devsw = args.mda_devsw;
	dev->si_drv0 = args.mda_unit;
	dev->si_drv1 = args.mda_si_drv1;
	dev->si_drv2 = args.mda_si_drv2;
	dev->si_cred = args.mda_cr;
	dev->si_uid = args.mda_uid;
	dev->si_gid = args.mda_gid;
	dev->si_mode = (mode_t)args.mda_mode;
	dev->si_refcount = (args.mda_flags & MAKEDEV_REF) != 0 ? 2 : 1;
	error = xg_fd_xnu_node_create(xc->xc_slot, args.mda_uid, args.mda_gid, args.mda_mode, dev->si_name,
	    &xc->xc_node);
	if (error != 0) {
		xg_fd_cdev_unname(xc);
		return (error);
	}
	dev_lock();
	LIST_INSERT_HEAD(&args.mda_devsw->d_devs, dev, si_list);
	dev_unlock();
	*dres = dev;
	return (0);
}

int
make_dev_s(struct make_dev_args *args, struct cdev **dres, const char *fmt, ...)
{
	va_list ap;
	int res;

	va_start(ap, fmt);
	res = xg_fd_make_dev_sv(args, dres, fmt, ap);
	va_end(ap);
	return (res);
}

int
make_dev_alias_p(int flags, struct cdev **cdev, struct cdev *pdev, const char *fmt, ...)
{
	struct xg_fd_cdev *xc, *px = (struct xg_fd_cdev *)pdev;
	struct cdev *dev;
	va_list ap;
	int error;

	KASSERT(pdev != NULL, ("make_dev_alias_p: pdev is NULL"));
	va_start(ap, fmt);
	error = xg_fd_cdev_new(flags, true, &xc, fmt, ap);
	va_end(ap);
	if (error != 0)
		return (error);
	dev = &xc->xc_cdev;
	dev->si_flags = SI_ALIAS | SI_NAMED;
	/* the alias node opens its parent: same XNU minor */
	xc->xc_slot = px->xc_slot;
	error = xg_fd_xnu_node_create(xc->xc_slot, pdev->si_uid, pdev->si_gid, pdev->si_mode, dev->si_name,
	    &xc->xc_node);
	if (error != 0) {
		xg_fd_cdev_unname(xc);
		return (error);
	}
	dev_lock();
	dev->si_parent = pdev;
	dev->si_flags |= SI_CHILD;
	LIST_INSERT_HEAD(&pdev->si_children, dev, si_siblings);
	dev_unlock();
	*cdev = dev;
	return (0);
}

void
destroy_dev(struct cdev *dev)
{
	struct xg_fd_cdev *xc = (struct xg_fd_cdev *)dev;
	struct cdev *child;
	bool last;

	KASSERT(dev->si_flags & SI_NAMED, ("destroy_dev on an unnamed cdev"));
	/* no new opens from here on */
	if (xc->xc_node != NULL) {
		xg_fd_xnu_node_remove(xc->xc_node);
		xc->xc_node = NULL;
	}
	for (;;) {
		dev_lock();
		child = LIST_FIRST(&dev->si_children);
		dev_unlock();
		if (child == NULL)
			break;
		destroy_dev(child);
	}
	xg_fd_lock();
	xc->xc_gone = true;
	LIST_REMOVE(xc, xc_all);
	if ((dev->si_flags & SI_ALIAS) == 0)
		xg_fd_slots[xc->xc_slot] = NULL;
	while (xc->xc_busy != 0) {
		xg_fd_unlock();
		pause("xgfddrn", hz / 100 + 1);
		xg_fd_lock();
	}
	xg_fd_unlock();
	dev_lock();
	dev->si_flags &= ~SI_NAMED;
	if ((dev->si_flags & SI_CHILD) != 0) {
		LIST_REMOVE(dev, si_siblings);
		dev->si_flags &= ~SI_CHILD;
	}
	if ((dev->si_flags & SI_ALIAS) == 0 && dev->si_devsw != NULL)
		LIST_REMOVE(dev, si_list);
	dev->si_devsw = NULL;
	dev->si_drv1 = NULL;
	dev->si_drv2 = NULL;
	dev_unlock();
	xg_fd_lock();
	last = --xc->xc_refs == 0;
	xg_fd_unlock();
	if (last)
		xg_fd_cdev_free(xc);
}

dev_t
dev2udev(struct cdev *x)
{
	if (x == NULL)
		return (NODEV);
	/* the XNU dev_t userland sees in st_rdev; an alias shares its parent's */
	return ((dev_t)xg_fd_xnu_udev(((struct xg_fd_cdev *)x)->xc_slot));
}

const char *
devtoname(struct cdev *cdev)
{
	return (cdev->si_name);
}

/* fileops for a cdevsw without d_fdopen: what devfs_ops_f does, forwarding to the driver */
static struct cdevsw *
xg_fd_devsw(struct file *fp, struct cdev **devp)
{
	*devp = fp->f_data;
	return ((*devp)->si_devsw);
}

static int
xg_fd_devfo_read(struct file *fp, struct uio *uio, struct ucred *cred, int flags, struct thread *td)
{
	struct cdevsw *csw;
	struct cdev *dev;

	csw = xg_fd_devsw(fp, &dev);
	if (csw == NULL)
		return (ENXIO);
	if (csw->d_read == NULL)
		return (ENODEV);
	return (csw->d_read(dev, uio, (fp->f_flag & FNONBLOCK) != 0 ? IO_NDELAY : 0));
}

static int
xg_fd_devfo_write(struct file *fp, struct uio *uio, struct ucred *cred, int flags, struct thread *td)
{
	struct cdevsw *csw;
	struct cdev *dev;

	csw = xg_fd_devsw(fp, &dev);
	if (csw == NULL)
		return (ENXIO);
	if (csw->d_write == NULL)
		return (ENODEV);
	return (csw->d_write(dev, uio, (fp->f_flag & FNONBLOCK) != 0 ? IO_NDELAY : 0));
}

static int
xg_fd_devfo_ioctl(struct file *fp, u_long com, void *data, struct ucred *cred, struct thread *td)
{
	struct cdevsw *csw;
	struct cdev *dev;

	csw = xg_fd_devsw(fp, &dev);
	if (csw == NULL)
		return (ENXIO);
	if (com == FIONBIO || com == FIOASYNC)
		return (0);
	if (csw->d_ioctl == NULL)
		return (ENODEV);
	return (csw->d_ioctl(dev, com, data, fp->f_flag, td));
}

static int
xg_fd_devfo_poll(struct file *fp, int events, struct ucred *cred, struct thread *td)
{
	struct cdevsw *csw;
	struct cdev *dev;

	csw = xg_fd_devsw(fp, &dev);
	if (csw == NULL)
		return (POLLHUP);
	if (csw->d_poll == NULL)	/* no_poll: always ready */
		return (events & (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM));
	return (csw->d_poll(dev, events, td));
}

static int
xg_fd_devfo_close(struct file *fp, struct thread *td)
{
	struct cdevsw *csw;
	struct cdev *dev;

	csw = xg_fd_devsw(fp, &dev);
	if (csw == NULL || csw->d_close == NULL)
		return (0);
	return (csw->d_close(dev, fp->f_flag, S_IFCHR, td));
}

static const struct fileops xg_fd_devfileops = {
	.fo_read = xg_fd_devfo_read,
	.fo_write = xg_fd_devfo_write,
	.fo_truncate = invfo_truncate,
	.fo_ioctl = xg_fd_devfo_ioctl,
	.fo_poll = xg_fd_devfo_poll,
	.fo_kqfilter = invfo_kqfilter,
	.fo_stat = xg_fd_badfo_stat,
	.fo_close = xg_fd_devfo_close,
	.fo_chmod = invfo_chmod,
	.fo_chown = invfo_chown,
	.fo_sendfile = invfo_sendfile,
	.fo_flags = DFLAG_PASSABLE,
};

/*
 * Open instances of the XNU clone nodes. The devfs clone callback reserves one per lookup (XI_LOOKUP), d_open makes
 * it XI_OPEN with its own struct file, d_close frees it. Allocation is round robin so a late DEVFS_CLONE_FREE for
 * an old instance cannot hit a fresh one.
 */
enum { XI_FREE, XI_LOOKUP, XI_OPENING, XI_OPEN };
struct xg_fd_inst {
	int		xi_state;
	int		xi_pid;		/* opener, owner of the private table */
	struct xg_fd_cdev *xi_cdev;
	struct file	*xi_fp;
};
static struct xg_fd_inst xg_fd_insts[XG_FD_NINST];
static int xg_fd_inst_next;

int
xg_fd_inst_alloc(int slot)
{
	struct xg_fd_cdev *xc;
	int i = 0, n;

	if (slot < 0 || slot >= XG_FD_NCDEV)
		return (-1);
	xg_fd_lock();
	xc = xg_fd_slots[slot];
	if (xc == NULL || xc->xc_gone) {
		xg_fd_unlock();
		return (-1);
	}
	for (n = 0; n < XG_FD_NINST; n++) {
		i = (xg_fd_inst_next + n) % XG_FD_NINST;
		if (xg_fd_insts[i].xi_state == XI_FREE)
			break;
	}
	if (n == XG_FD_NINST) {
		xg_fd_unlock();
		return (-1);
	}
	xg_fd_inst_next = (i + 1) % XG_FD_NINST;
	xg_fd_insts[i].xi_state = XI_LOOKUP;
	xg_fd_insts[i].xi_cdev = xc;
	xc->xc_refs++;
	xg_fd_unlock();
	return (i);
}

void
xg_fd_inst_free(int inst)
{
	struct xg_fd_inst *xi;
	struct xg_fd_cdev *xc = NULL;

	if (inst < 0 || inst >= XG_FD_NINST)
		return;
	xi = &xg_fd_insts[inst];
	xg_fd_lock();
	/* only a lookup that was never opened; an open instance is freed by its close */
	if (xi->xi_state == XI_LOOKUP) {
		xc = xi->xi_cdev;
		xi->xi_cdev = NULL;
		xi->xi_state = XI_FREE;
	}
	xg_fd_unlock();
	if (xc != NULL)
		xg_fd_cdev_rele(xc);
}

int
xg_fd_dev_open(int inst, int fflags)
{
	struct thread *td = curthread;
	struct xg_fd_inst *xi;
	struct xg_fd_cdev *xc;
	struct xg_fd_tbl *t, *nt;
	struct cdevsw *csw;
	struct cdev *dev;
	struct file *fp = NULL;
	int error;

	if (inst < 0 || inst >= XG_FD_NINST)
		return (ENXIO);
	xi = &xg_fd_insts[inst];
	nt = malloc(sizeof(*nt), M_XGFD, M_WAITOK | M_ZERO);
	if (nt == NULL)
		return (ENOMEM);
	xg_fd_lock();
	if (xi->xi_state != XI_LOOKUP) {
		error = xi->xi_state == XI_FREE ? ENXIO : EBUSY;
		xg_fd_unlock();
		free(nt, M_XGFD);
		return (error);
	}
	if (xi->xi_cdev->xc_gone) {
		/* destroyed between lookup and open: give the reservation back */
		xc = xi->xi_cdev;
		xi->xi_cdev = NULL;
		xi->xi_state = XI_FREE;
		xg_fd_unlock();
		free(nt, M_XGFD);
		xg_fd_cdev_rele(xc);
		return (ENXIO);
	}
	xi->xi_state = XI_OPENING;
	xc = xi->xi_cdev;
	xc->xc_busy++;
	xg_fd_unlock();

	dev = &xc->xc_cdev;
	csw = dev->si_devsw;
	error = _falloc_noinstall(td, &fp, 1);
	if (error == 0) {
		fp->f_flag = fflags & FMASK;
		fp->f_vnode = &xc->xc_vnode;
		if (csw == NULL)
			error = ENXIO;
		else if (csw->d_fdopen != NULL)
			error = csw->d_fdopen(dev, fp->f_flag, td, fp);
		else if (csw->d_open != NULL)
			error = csw->d_open(dev, fp->f_flag, S_IFCHR, td);
		/* d_fdopen returns ENXIO once it has taken the file over (kern_openat treats that as success) */
		if (error == ENXIO && fp->f_ops != &xg_fd_badfileops)
			error = 0;
		if (error == 0 && fp->f_ops == &xg_fd_badfileops)
			finit(fp, fp->f_flag, DTYPE_VNODE, dev, &xg_fd_devfileops);
	}

	xg_fd_lock();
	xc->xc_busy--;
	if (error != 0) {
		xi->xi_state = XI_FREE;
		xi->xi_cdev = NULL;
		xg_fd_unlock();
		free(nt, M_XGFD);
		if (fp != NULL)
			fdrop(fp, td);
		xg_fd_cdev_rele(xc);
		return (error);
	}
	xi->xi_fp = fp;
	xi->xi_pid = xg_fd_xnu_pid();
	xi->xi_state = XI_OPEN;
	t = xg_fd_tbl_find(xi->xi_pid);
	if (t == NULL) {
		t = nt;
		nt = NULL;
		t->ft_pid = xi->xi_pid;
		LIST_INSERT_HEAD(&xg_fd_tbls, t, ft_link);
	}
	t->ft_devrefs++;
	xg_fd_unlock();
	free(nt, M_XGFD);
	return (0);
}

int
xg_fd_dev_close(int inst)
{
	struct thread *td = curthread;
	struct xg_fd_inst *xi;
	struct xg_fd_cdev *xc;
	struct xg_fd_tbl *t, *dead = NULL;
	struct file *fp;
	int error;

	if (inst < 0 || inst >= XG_FD_NINST)
		return (0);
	xi = &xg_fd_insts[inst];
	xg_fd_lock();
	if (xi->xi_state != XI_OPEN) {
		xg_fd_unlock();
		return (0);
	}
	fp = xi->xi_fp;
	xc = xi->xi_cdev;
	t = xg_fd_tbl_find(xi->xi_pid);
	if (t != NULL && --t->ft_devrefs == 0) {
		LIST_REMOVE(t, ft_link);
		dead = t;
	}
	xi->xi_fp = NULL;
	xi->xi_cdev = NULL;
	xi->xi_state = XI_FREE;
	xg_fd_unlock();

	if (xg_fd_map_close != NULL)
		xg_fd_map_close(fp);
	error = fdrop(fp, td);
	if (dead != NULL)
		xg_fd_tbl_close(dead, td);
	xg_fd_cdev_rele(xc);
	return (error);
}

int
xg_fd_inst_fget(int inst, struct file **fpp)
{
	struct file *fp = NULL;

	if (inst < 0 || inst >= XG_FD_NINST)
		return (EBADF);
	xg_fd_lock();
	if (xg_fd_insts[inst].xi_state == XI_OPEN && fhold(xg_fd_insts[inst].xi_fp))
		fp = xg_fd_insts[inst].xi_fp;
	xg_fd_unlock();
	if (fp == NULL)
		return (EBADF);
	*fpp = fp;
	return (0);
}

/* XNU passes its own idea of O_NONBLOCK with every call; keep the FreeBSD file in step */
static void
xg_fd_sync_nonblock(struct file *fp, int nonblock)
{
	if (nonblock)
		atomic_set_int(&fp->f_flag, FNONBLOCK);
	else
		atomic_clear_int(&fp->f_flag, FNONBLOCK);
}

/*
 * Device mmap(2) is not open to kexts, so GEM objects are mapped through ioctls in group 'N' (NullMoth's Darwin
 * libdrm ABI). The drm glue sets these; it gets the LinuxKPI file and uses the FreeBSD file only as a key.
 * Unlike mmap(2), a mapping ends when its node is closed (Mesa keeps the node open while it maps).
 */
extern struct fileops linuxfileops;
int (*xg_fd_map_ioctl)(void *lfile, void *key, unsigned long cmd, void *data);
void (*xg_fd_map_close)(void *key);

int
xg_fd_dev_ioctl(int inst, unsigned long cmd, void *data, int fflags)
{
	struct thread *td = curthread;
	struct file *fp;
	int error;

	if (xg_fd_inst_fget(inst, &fp) != 0)
		return (ENXIO);
	/* group 'N', plus DRM_IOCTL_GET_PCIINFO ('d' 0x15), which drm-kmod declares but does not implement */
	if ((IOCGROUP(cmd) == 'N' || (IOCGROUP(cmd) == 'd' && (cmd & 0xff) == 0x15)) && xg_fd_map_ioctl != NULL &&
	    fp->f_ops == &linuxfileops) {
		error = xg_fd_map_ioctl(fp->f_data, fp, cmd, data);
		if (error != ENOTTY) {
			fdrop(fp, td);
			return (error);
		}
	}
	if (cmd == FIONBIO)
		xg_fd_sync_nonblock(fp, *(int *)data != 0);
	else
		xg_fd_sync_nonblock(fp, (fflags & FNONBLOCK) != 0);
	error = fo_ioctl(fp, cmd, data, td->td_ucred, td);
	fdrop(fp, td);
	return (error);
}

int
xg_fd_dev_rw(int inst, int write, unsigned long long uaddr, unsigned long len, long long *offset, int nonblock,
    unsigned long *done)
{
	struct thread *td = curthread;
	struct iovec iov;
	struct uio uio;
	struct file *fp;
	int error;

	*done = 0;
	if (xg_fd_inst_fget(inst, &fp) != 0)
		return (ENXIO);
	xg_fd_sync_nonblock(fp, nonblock);
	/* the user address goes through as is: LinuxKPI's read hands iov_base to copy_to_user */
	iov.iov_base = (void *)(uintptr_t)uaddr;
	iov.iov_len = len;
	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_offset = *offset;
	uio.uio_resid = len;
	uio.uio_segflg = UIO_USERSPACE;
	uio.uio_rw = write ? UIO_WRITE : UIO_READ;
	uio.uio_td = td;
	if (write)
		error = fo_write(fp, &uio, td->td_ucred, 0, td);
	else
		error = fo_read(fp, &uio, td->td_ucred, 0, td);
	*done = len - uio.uio_resid;
	*offset = uio.uio_offset;
	fdrop(fp, td);
	return (error);
}

int
xg_fd_dev_poll(int inst, int which)
{
	struct thread *td = curthread;
	struct file *fp;
	int events, revents;

	if (xg_fd_inst_fget(inst, &fp) != 0)
		return (1);	/* gone: let select return and the read fail */
	if (which == FREAD)
		events = POLLIN | POLLRDNORM;
	else if (which == FWRITE)
		events = POLLOUT | POLLWRNORM;
	else
		events = POLLPRI | POLLRDBAND;
	revents = fo_poll(fp, events, td->td_ucred, td);
	fdrop(fp, td);
	return ((revents & (events | POLLHUP | POLLERR | POLLNVAL)) != 0);
}

/*
 * select bookkeeping. The XNU select bridge records the waiter on its own selinfo before it polls, so there is
 * nothing to record here, and any wakeup re-polls every waiting instance (select allows spurious wakeups).
 */
void
xg_fd_selrecord(struct thread *td, struct selinfo *sip)
{
}

void
xg_fd_selwakeup(struct selinfo *sip)
{
	xg_fd_xnu_selwakeup_all();
}

/* eventfd, after sys_eventfd.c; XNU has none, so only xg_fd_eventfd_create makes one */
struct eventfd {
	eventfd_t	efd_count;
	uint32_t	efd_flags;
	struct mtx	efd_lock;
	struct cv	efd_cv;
	u_int		efd_refcount;
};

static const struct fileops xg_fd_eventfdops;

struct eventfd *
eventfd_get(struct file *fp)
{
	struct eventfd *efd;

	if (fp->f_data == NULL || fp->f_ops != &xg_fd_eventfdops)
		return (NULL);
	efd = fp->f_data;
	refcount_acquire(&efd->efd_refcount);
	return (efd);
}

void
eventfd_put(struct eventfd *efd)
{
	if (!refcount_release(&efd->efd_refcount))
		return;
	cv_destroy(&efd->efd_cv);
	mtx_destroy(&efd->efd_lock);
	free(efd, M_XGFD);
}

void
eventfd_signal(struct eventfd *efd)
{
	mtx_lock(&efd->efd_lock);
	if (efd->efd_count < UINT64_MAX)
		efd->efd_count++;
	cv_broadcast(&efd->efd_cv);
	mtx_unlock(&efd->efd_lock);
}

/* one 8-byte transfer to or from a single-iovec uio */
static int
xg_fd_uio8(eventfd_t *v, struct uio *uio)
{
	struct iovec *iov = uio->uio_iov;
	int error;

	if (uio->uio_iovcnt != 1 || iov->iov_len < sizeof(*v))
		return (EINVAL);
	if (uio->uio_segflg == UIO_USERSPACE)
		error = uio->uio_rw == UIO_READ ? copyout(v, iov->iov_base, sizeof(*v)) :
		    copyin(iov->iov_base, v, sizeof(*v));
	else {
		if (uio->uio_rw == UIO_READ)
			memcpy(iov->iov_base, v, sizeof(*v));
		else
			memcpy(v, iov->iov_base, sizeof(*v));
		error = 0;
	}
	if (error == 0) {
		iov->iov_base = (char *)iov->iov_base + sizeof(*v);
		iov->iov_len -= sizeof(*v);
		uio->uio_resid -= sizeof(*v);
		uio->uio_offset += sizeof(*v);
	}
	return (error);
}

static int
xg_fd_eventfd_read(struct file *fp, struct uio *uio, struct ucred *cred, int flags, struct thread *td)
{
	struct eventfd *efd = fp->f_data;
	eventfd_t count;
	int error = 0;

	if (uio->uio_resid < (ssize_t)sizeof(eventfd_t))
		return (EINVAL);
	mtx_lock(&efd->efd_lock);
	while (error == 0 && efd->efd_count == 0) {
		if ((fp->f_flag & FNONBLOCK) != 0) {
			mtx_unlock(&efd->efd_lock);
			return (EAGAIN);
		}
		error = cv_wait_sig(&efd->efd_cv, &efd->efd_lock);
	}
	if (error != 0) {
		mtx_unlock(&efd->efd_lock);
		return (error);
	}
	if ((efd->efd_flags & EFD_SEMAPHORE) != 0) {
		count = 1;
		efd->efd_count--;
	} else {
		count = efd->efd_count;
		efd->efd_count = 0;
	}
	cv_broadcast(&efd->efd_cv);
	mtx_unlock(&efd->efd_lock);
	return (xg_fd_uio8(&count, uio));
}

static int
xg_fd_eventfd_write(struct file *fp, struct uio *uio, struct ucred *cred, int flags, struct thread *td)
{
	struct eventfd *efd = fp->f_data;
	eventfd_t count;
	int error;

	if (uio->uio_resid < (ssize_t)sizeof(eventfd_t))
		return (EINVAL);
	error = xg_fd_uio8(&count, uio);
	if (error != 0)
		return (error);
	if (count == UINT64_MAX)
		return (EINVAL);
	mtx_lock(&efd->efd_lock);
	while (UINT64_MAX - efd->efd_count <= count) {
		if ((fp->f_flag & FNONBLOCK) != 0) {
			mtx_unlock(&efd->efd_lock);
			uio->uio_resid += sizeof(eventfd_t);
			return (EAGAIN);
		}
		error = cv_wait_sig(&efd->efd_cv, &efd->efd_lock);
		if (error != 0) {
			mtx_unlock(&efd->efd_lock);
			return (error);
		}
	}
	efd->efd_count += count;
	cv_broadcast(&efd->efd_cv);
	mtx_unlock(&efd->efd_lock);
	return (0);
}

static int
xg_fd_eventfd_ioctl(struct file *fp, u_long cmd, void *data, struct ucred *cred, struct thread *td)
{
	return (cmd == FIONBIO || cmd == FIOASYNC ? 0 : ENOTTY);
}

static int
xg_fd_eventfd_poll(struct file *fp, int events, struct ucred *cred, struct thread *td)
{
	struct eventfd *efd = fp->f_data;
	int revents = 0;

	mtx_lock(&efd->efd_lock);
	if ((events & (POLLIN | POLLRDNORM)) != 0 && efd->efd_count > 0)
		revents |= events & (POLLIN | POLLRDNORM);
	if ((events & (POLLOUT | POLLWRNORM)) != 0 && UINT64_MAX - 1 > efd->efd_count)
		revents |= events & (POLLOUT | POLLWRNORM);
	mtx_unlock(&efd->efd_lock);
	return (revents);
}

static int
xg_fd_eventfd_close(struct file *fp, struct thread *td)
{
	eventfd_put(fp->f_data);
	return (0);
}

static const struct fileops xg_fd_eventfdops = {
	.fo_read = xg_fd_eventfd_read,
	.fo_write = xg_fd_eventfd_write,
	.fo_truncate = invfo_truncate,
	.fo_ioctl = xg_fd_eventfd_ioctl,
	.fo_poll = xg_fd_eventfd_poll,
	.fo_kqfilter = invfo_kqfilter,
	.fo_stat = xg_fd_badfo_stat,
	.fo_close = xg_fd_eventfd_close,
	.fo_chmod = invfo_chmod,
	.fo_chown = invfo_chown,
	.fo_sendfile = invfo_sendfile,
	.fo_flags = DFLAG_PASSABLE,
};

int
xg_fd_eventfd_create(struct thread *td, unsigned int initval, int flags, int *fdp)
{
	struct eventfd *efd;
	struct file *fp;
	int error, fflags;

	if ((flags & ~(EFD_SEMAPHORE | EFD_NONBLOCK | EFD_CLOEXEC)) != 0)
		return (EINVAL);
	efd = malloc(sizeof(*efd), M_XGFD, M_WAITOK | M_ZERO);
	if (efd == NULL)
		return (ENOMEM);
	error = falloc_caps(td, &fp, fdp, 0, NULL);
	if (error != 0) {
		free(efd, M_XGFD);
		return (error);
	}
	efd->efd_flags = flags;
	efd->efd_count = initval;
	mtx_init(&efd->efd_lock, "eventfd", NULL, MTX_DEF);
	cv_init(&efd->efd_cv, "efd");
	refcount_init(&efd->efd_refcount, 1);
	fflags = FREAD | FWRITE;
	if ((flags & EFD_NONBLOCK) != 0)
		fflags |= FNONBLOCK;
	finit(fp, fflags, DTYPE_EVENTFD, efd, &xg_fd_eventfdops);
	fdrop(fp, td);
	return (0);
}
