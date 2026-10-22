/*
 * XeGfx: the bridge between the FreeBSD-side device and file code (xg_fd.c) and the XNU cdevsw (xg_fd.cpp).
 * Plain C types only, so both sides can include it.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#ifndef _XG_FD_H_
#define _XG_FD_H_

#ifdef __cplusplus
extern "C" {
#endif

/*
 * XNU minor layout of XeGfx nodes: a devfs node (cdev or alias) has its cdev slot as minor; the clone function
 * gives every open its own instance minor, XG_FD_INST_FLAG | instance << XG_FD_SLOT_BITS | slot. The slot stays in
 * the low bits so fstat on an open fd still names its node (libdrm maps it to card%d/renderD%d through /dev/dri).
 */
#define XG_FD_NCDEV		1024
#define XG_FD_NINST		512
#define XG_FD_INST_FLAG		0x800000
#define XG_FD_SLOT_BITS		10
#define XG_FD_INST_MINOR(inst, slot)	(XG_FD_INST_FLAG | (inst) << XG_FD_SLOT_BITS | (slot))
#define XG_FD_MINOR_INST(m)	(((m) & ~XG_FD_INST_FLAG) >> XG_FD_SLOT_BITS)

/* private descriptor numbers start here, far above any real fd, so a stray close(2) on one is only EBADF */
#define XG_FD_BASE		0x40000000

/* FreeBSD side, called from the XNU cdevsw and the devfs clone function */
int	xg_fd_inst_alloc(int slot);
void	xg_fd_inst_free(int inst);
int	xg_fd_dev_open(int inst, int fflags);
int	xg_fd_dev_close(int inst);
int	xg_fd_dev_ioctl(int inst, unsigned long cmd, void *data, int fflags);
int	xg_fd_dev_rw(int inst, int write, unsigned long long uaddr, unsigned long len, long long *offset,
	    int nonblock, unsigned long *done);
int	xg_fd_dev_poll(int inst, int which);

/* XNU side, called from xg_fd.c */
int	xg_fd_xnu_node_create(int slot, unsigned int uid, unsigned int gid, int mode, const char *name,
	    void **handlep);
void	xg_fd_xnu_node_remove(void *handle);
unsigned int xg_fd_xnu_udev(int slot);
int	xg_fd_xnu_pid(void);
int	xg_fd_xnu_issuser(void);
void	xg_fd_xnu_selwakeup_all(void);
void	xg_fd_xnu_fini(void);

#if defined(_KERNEL) && !defined(__cplusplus)
struct thread;
struct file;
struct selinfo;

/* held file of an open device instance, for the mmap ioctl path (the caller fdrops it) */
int	xg_fd_inst_fget(int inst, struct file **fpp);
/* a private eventfd descriptor in the calling process's table */
int	xg_fd_eventfd_create(struct thread *td, unsigned int initval, int flags, int *fdp);
/* FreeBSD selrecord/selwakeup for XeGfx; XNU exports functions of the same names with another ABI */
void	xg_fd_selrecord(struct thread *td, struct selinfo *sip);
void	xg_fd_selwakeup(struct selinfo *sip);
#endif

/* GEM map ioctls ('N' group) and their cleanup when a node closes; set by the drm glue, NULL until then */
extern int (*xg_fd_map_ioctl)(void *lfile, void *key, unsigned long cmd, void *data);
extern void (*xg_fd_map_close)(void *key);

#ifdef __cplusplus
}
#endif
#endif
