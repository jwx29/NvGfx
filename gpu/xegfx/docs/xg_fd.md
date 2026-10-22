# xg_fd: character devices and file descriptors

`shim/src/fbsd/xg_fd.c` (FreeBSD side) and `shim/src/xnu/xg_fd.cpp` (XNU side) give LinuxKPI the FreeBSD device
and file API it uses for the DRM nodes, dma-buf and sync_file descriptors. `shim/include/xg_fd.h` is the bridge
between the two, plain C types only.

## Device nodes

`make_dev_s()` allocates an `xg_fd_cdev`: the FreeBSD `struct cdev`, a `struct vnode` that LinuxKPI uses as the
inode (`v_rdev` for `iminor()`, `vref`/`vrele` per open file), a slot number and the XNU devfs handle. The cdev goes
on its cdevsw's `d_devs` list under `dev_lock()`, as in `kern_conf.c`, because `linux_find_cdev()` walks that list.
Names go through FreeBSD's `prep_devname()` rules, and a duplicate name is `EEXIST`.

On XNU every node is a devfs clone node of one BSD cdevsw (`cdevsw_add(-24, ...)`, registered on the first
`make_dev_s`). The minor of a node is its slot. An alias (`make_dev_alias_p`, used for `dri/card0` and
`dri/renderD128`) is another devfs node with its parent's slot, so opening it opens the parent.

Each lookup of a clone node calls the clone function, which reserves an instance and returns minor
`XG_FD_INST_FLAG | instance << 10 | slot` (`XG_FD_INST_MINOR`). The slot stays in the low bits because a `stat` of
the node and every `fstat` of an open fd report a clone minor, and libdrm on Darwin finds the node by its slot (the
DRM minor: 0 for `card0`, 128 for `renderD128`). `d_open` on that minor then does what FreeBSD's `devfs_open` does: `falloc_noinstall`,
set `f_flag` and `f_vnode`, call the cdevsw's `d_fdopen` (LinuxKPI's `linux_dev_fdopen`) or `d_open`. An `ENXIO`
from `d_fdopen` after it replaced `f_ops` is success, as in `kern_openat`. So every open has its own `struct file`
and its own `linux_file`, which is what DRM needs for per-open `drm_file` state. `d_close` of the instance drops
that file (`linux_file_close`, then the driver's release).

| XNU entry | FreeBSD call |
|---|---|
| `d_open` | `d_fdopen` / `d_open` of the cdevsw |
| `d_close` | `fdrop` of the instance's file |
| `d_ioctl` | `fo_ioctl` (XNU already copied the argument in, as FreeBSD does) |
| `d_read` / `d_write` | `fo_read` / `fo_write` with a one-iovec `UIO_USERSPACE` uio; LinuxKPI passes `iov_base` to `copy_to_user`, which ends in XNU `copyout` for the calling task |
| `d_select` | XNU `selrecord` on the instance's selinfo, then `fo_poll` |
| `d_mmap` | `eno_mmap`, see below |

`O_NONBLOCK` is copied from XNU's flags into `f_flag` on every call (and on `FIONBIO`), because XNU's `fcntl` never
reaches the FreeBSD file. FreeBSD errnos that XNU numbers differently (82 to 97) are translated on the way out.

A cdevsw without `d_fdopen` gets `xg_fd_devfileops`, which forwards read, write, ioctl, poll and close to the
driver like `devfs_ops_f`.

`destroy_dev()` removes the XNU node (no new opens), destroys aliases, waits for opens in progress, takes the cdev
off the lists and clears `si_devsw`/`si_drv1`/`si_drv2` as FreeBSD does. Open files keep working through LinuxKPI's
own teardown handling (`LDEV_SI_DTR`, `dummy_ldev`). The `xg_fd_cdev` memory is freed when the last open instance is
closed; if the vnode still has references then (a file held elsewhere, such as an mmap's `vm_file`), it is leaked
with a log line rather than freed under a live file.

## File descriptors

XNU has no KPI that installs a file in a process's descriptor table. Descriptors that LinuxKPI hands out
(`get_unused_fd_flags`, `fd_install`, dma-buf and sync_file export) therefore live in a private table per XNU pid.
Numbers start at `XG_FD_BASE` (0x40000000), far above any real descriptor, so a `close(2)` on one from userspace is
a harmless `EBADF` and never closes another file. They mean something only to XeGfx ioctls of the same process:
`fget`, `fget_unlocked`, `fdclose`, `finstall` and `falloc_caps` all work on that table. A table is closed (every
file dropped) when the last device instance its process opened is closed. Rights (`cap_rights_t`) are not checked
and `O_CLOEXEC` is ignored.

Consequences for userspace (the Mesa side): such a descriptor cannot be `close`d, `dup`ed, `mmap`ed, polled or sent
to another process with real syscalls. The XeGfx userspace layer needs an ioctl to close one, and buffer sharing
between processes needs another channel (IOSurface or a global handle), not dma-buf fds. A forked child gets its
own empty table.

## What is stubbed or limited

- mmap: XNU refuses `mmap(2)` on character devices, so there is no `d_mmap`. Mapping a GEM object has to be an
  XeGfx ioctl (or an IOUserClient memory type) that calls `fo_mmap`/LinuxKPI's `linux_file_mmap_single` and maps the
  result into the task with Mach VM. `xg_fd_inst_fget()` returns the instance's file for that path. Needed by X4.
- kqueue on the device node: not wired (`fo_kqfilter` exists in LinuxKPI, no cdevsw hook). Not needed before X6.
- eventfd: XNU has none. `eventfd_get/put/signal` are real, on a FreeBSD-style `struct eventfd`; the only creator is
  `xg_fd_eventfd_create()` (private descriptor), for a future XeGfx ioctl.
- `priv_check()` maps every privilege to "is superuser" (`kauth_cred_issuser`).
- `lim_cur_proc()` reports `RLIM_INFINITY`, except `RLIMIT_NOFILE`, which is the private table limit (65536).
- `fgetown()` reads `sio_pgid` without `SIGIO_LOCK`.
- File credentials are the shim's static ones (`td_ucred`), not reference counted.

## Assumptions to check on the machine

- XNU's `struct selinfo` is opaque to kexts. Each instance has a zeroed 128-byte, 16-aligned buffer for it, on the
  understanding that `selrecord` initialises a zeroed selinfo on first use and that the real struct (a waitq, a
  klist and a flag word) is well below 128 bytes. Check against the XNU source of this build before relying on
  select.
- `DEVFS_CLONE_FREE` is taken as "a lookup that was never opened went away"; an open instance is freed by
  `d_close`. Instances are handed out round robin so a late `DEVFS_CLONE_FREE` cannot hit a fresh reservation.
  If XNU never sends it for lookups without an open (`stat` of a node), those reservations leak; there are 512.

## Needs from other areas

- `dev_lock()`/`dev_unlock()` (misc): a sleepable mutex. xg_fd never holds its own lock across them.
- `vref()`/`vrele()` (misc): only count `v_usecount` on XeGfx vnodes, never free or recycle them; xg_fd owns that
  memory and reads `v_usecount` before freeing.
- `selrecord()`/`selwakeup()`: XNU exports functions with these names but another ABI (`proc_t`, XNU's
  `struct selinfo`), so FreeBSD-side callers must not link to them. A shim `sys/selinfo.h` override should map
  `selrecord` to `xg_fd_selrecord` and `selwakeup`/`selwakeuppri` to `xg_fd_selwakeup`, which wake every waiting
  XeGfx select (select tolerates spurious wakeups). The same name clash applies to other symbols that
  `tools/kpi_split.py` counts as provided by XNU only because the names match.
- `xg_fd_xnu_fini()` should be called from the kext's stop path once teardown exists, after every node is destroyed.
