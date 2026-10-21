# Misc area (xg_misc)

Files: `shim/src/fbsd/xg_misc.c` (FreeBSD side), `shim/src/xnu/xg_misc.cpp` (XNU side, firmware only),
`shim/include/xg_misc.h` (the bridge and the hooks other areas use). The symbols are listed in
`docs/areas/misc.txt`.

## Setup

`xg_misc_init()` initialises the area's locks and registers every static sysctl oid. The kext start should call it
after `xg_kern_init()`. Every entry point that needs it also calls it lazily, so the order does not matter for
correctness. One thing does depend on order: tunables are loaded when an oid is registered, so the init has to run
before any module's MOD_LOAD. `module_register_init()` calls it first for that reason.

## sysctl

This is a private in-memory tree. No sysctl syscall reaches it. The code is FreeBSD's `kern_sysctl.c` cut down:
the RB tree per node, OID_AUTO numbering, contexts, refcounted nodes, and removal with recursion and the
`sysctl_ctx_free` dry run. One sx lock covers the whole tree.

- Static oids (`SYSCTL_INT` and friends, plus the `hw`, `dev` and `compat` root nodes defined here) sit in the
  `sysctl_set` linker set. `sys/linker_set.h` puts every set in `__DATA,__xg_set`. The walk uses ld64's
  `section$start$__DATA$__xg_set` and `section$end$__DATA$__xg_set` symbols, so it needs no help from other areas.
- Tunables (CTLFLAG_TUN) come from boot-args under the oid's full dotted name, with a leading `hw.` dropped (see
  `xg_kern.c`). Example: `compat.linuxkpi.i915_enable_guc=3`. Numeric types are read with `getenv_long`, because
  `PE_parse_boot_argn` stores a numeric value in binary, and strings with `kern_getenv`.
- XNU exports `sysctl__children`, `sysctl_handle_int`, `sysctl_handle_long`, `sysctl_handle_string` and
  `sysctl_handle_opaque`, but with XNU's own struct layouts. `tools/kpi_split.py` therefore counts them as already
  provided. The FreeBSD versions are defined here, so the kext's static link binds to these instead.
- `oid_running` never goes up, since nothing outside calls the handlers. Removal therefore has nothing to drain.

## Symbols defined here that XNU also exports (wrong ABI)

Besides the sysctl ones above, xg_misc.c defines the whole FreeBSD `sbuf(9)` API (`sbuf_new`, `_delete`,
`_finish`, `_data`, `_len`, `_clear`, `_setpos`, `_bcat`, `_bcpy`, `_cat`, `_cpy`, `_putc`, `_printf`,
`_vprintf`, `_error`, `_done`). It also defines `knote`. XNU's versions take XNU structs (its `struct sbuf` differs, and its `knote` takes
a `klist`), so LinuxKPI must never bind to them. The catch: this applies to the whole kext. XNU-side `.cpp` code
in XeGfx that wants XNU's own `sysctl_handle_*`, `sysctl__children`, `sbuf_*` or `knote` would get the FreeBSD ones.
None does today. The clean long-term fix is renaming them in shim header overrides (`#define sbuf_new
xg_sbuf_new`, and so on). Other XNU exports with the same problem belong to other areas and are listed in the report:
`eventhandler_*`, `kmem_free`, `vm_page_*`, `pmap_*_page`, `log`, `osrelease`, `copyin`/`copyout`.

## counter(9)

A counter is one 64-bit cell, made with `counter_u64_alloc` and read atomically by `sysctl_handle_counter_u64`.
**Header change needed:** amd64's `machine/counter.h` adds through `%gs:(c)`, which on XNU indexes XNU's per-CPU
data and would corrupt it. The only user today is `counter_u64_add(lkpi_pci_nseg1_fail, 1)` in `linux_pci.c`, on
a DMA failure path. A `shim/include/machine/counter.h` override should provide:

```c
#define counter_u64_add(c, i)	atomic_add_64((volatile uint64_t *)(c), (i))
#define counter_u64_fetch(c)	atomic_load_64((volatile uint64_t *)(c))
#define counter_u64_zero(c)	atomic_store_64((volatile uint64_t *)(c), 0)
```

## Group taskqueues

The queues come from the sched area (`xg_sched_gtq_create`, `_drain`, `_drain_all`), and `grouptaskqueue_enqueue` is
sched's too, safe from interrupt context. This file holds only `struct taskqgroup`: one slot per CPU, each slot's
queue made on the first attach to it.

- `qgroup_softirq` is created on first use, because `TASKQGROUP_DEFINE`'s SYSINIT does not run.
- `taskqgroup_detach` does FreeBSD's `grouptask_block()` without the queue's private lock. It sets
  TASK_NOENQUEUE, drains, and repeats if a racing enqueue dropped the flag. The drain takes the queue lock after
  the write, so a lost flag is always visible afterwards. An `xg_sched_gtq_block(q, t)` in sched would make this a
  single call.

## Firmware

`firmware_get_flags(name)` loads `Contents/Resources/<name>` through `OSKextRequestResource`. The XNU side waits
for kernelmanagerd's answer for up to 30 s, copies the data into an IOMalloc buffer and frees the request with a
two-reference handoff, so a late callback after a timeout is safe. Images are cached by name and refcounted.
`firmware_put(.., FIRMWARE_UNLOAD)` frees the image after the last reference goes.

- Resources is flat, so names with a `/` fail at once. LinuxKPI then retries with the basename
  (`i915/mtl_guc_70.bin` becomes `mtl_guc_70.bin`), which is the name to ship under Resources.
- kernelmanagerd answers only after the kext's start routine has returned. A firmware request made from `_start`
  waits out the full 30 s timeout. The i915 probe has to run from an IOService start or a thread.
- Untested so far: whether kernelmanagerd serves resources for a kext loaded from the auxiliary KC.

## select and knlist

There is no FreeBSD kqueue on XNU. `knlist_*` keep FreeBSD's list and lock semantics. `knote()` runs each note's
`f_event`, but activation has nowhere to go. `seldrain` calls `xg_fd_selwakeup`. `selrecord` and `selwakeup` are
deliberately not defined here: `xg_fd.cpp` calls XNU's functions of those names, and a FreeBSD definition anywhere
in the kext would capture those calls. FreeBSD callers need the `sys/selinfo.h` rename that `docs/xg_fd.md`
describes.

## Deliberately minimal

- `smp_rendezvous` runs setup, action and teardown on the calling CPU only, with interrupts off. The SDK has no
  cross-call KPI (`mp_rendezvous` is exported but not declared). The only caller is `on_each_cpu` in
  `vlv_sideband.c`, which is Valleyview, not Xe.
- `mi_switch` drops the thread lock. For SWT_RELINQUISH it sleeps one tick (`pause`), as `docs/xg_sched.md` asks,
  because a bare yield can keep a lower-priority RCU reader off the CPU. Other switches use `thread_block(NULL)`.
  `kern_yield` sleeps the same tick and leaves the priority alone, since XNU picks it.
- `XG_PCPU_domain()` returns 0 and the domainsets describe a single domain 0. `PCPU_GET(domain)` in
  `linux_page.c` reaches this function.
- vnodes: XeGfx files have no FreeBSD vnode. `vref` and `vrele` count `v_usecount`, `_vn_lock` succeeds, and
  `vn_fill_kinfo_vnode` returns EOPNOTSUPP. The `vop_stat_desc` and `vop_unlock_desc` descriptors are filled in
  like `vnode_if.c` would.
- `fsetown` accepts only pgid 0 (clear the owner) and returns EOPNOTSUPP otherwise, because no FreeBSD signal
  reaches an XNU process.
- `vga_pci_map_bios` returns NULL with size 0. The VBT comes from the ACPI OpRegion on UEFI laptops, and i915
  handles a missing option ROM.
- vt: there is no FreeBSD console. `vt_allocate` and `vt_deallocate` succeed and do nothing, `vt_fb_ioctl` returns
  ENOTTY and `vt_fb_mmap` returns EINVAL.
- debugfs: the create calls return `ERR_PTR(-ENODEV)`, which is what Linux returns with CONFIG_DEBUG_FS off. The
  void ones do nothing. The same goes for `intel_gt_debugfs_register` and `intel_gt_debugfs_register_files`.
- i2c: `lkpi_i2c_add_adapter` and `lkpi_i2c_del_adapter` succeed without a FreeBSD iicbus, and `i2c_transfer`
  uses the adapter's own `master_xfer` (GMBUS has one). `lkpi_i2cbb_transfer` for bit-banged adapters returns
  -ENXIO until external displays (X6).
- `root_bus` is NULL until the PCI area sets it.
- `doadump` returns ENXIO (no dump device).

## Stub

- `cdev_mgtdev_pager_free_pages` calls `xg_unimpl`. It needs the vm area's device pager objects and is first
  reached when GEM objects are mmapped to userspace (X5).
