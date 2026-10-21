# xg_sched: threads and scheduling

`shim/src/fbsd/xg_sched.c` implements the FreeBSD scheduler and thread calls that LinuxKPI and drm-kmod need, on XNU. The XNU calls it makes beyond `xg_xnu.h` are in `shim/include/xg_sched_xnu.h`; what other shim files can call is in `shim/include/xg_sched.h`.

## Thread registry

FreeBSD finds threads through a tid hash and walks `proc->p_threads`; LinuxKPI uses both (`tdfind`, `FOREACH_THREAD_IN_PROC`). Thread shadows come from `xg_curthread()` in `xg_kern.c`, so `xg_sched_td_register(td)` puts a shadow on proc0's thread list and into a 256-bucket tid hash. It never sleeps and works from any context: it takes a raw spin lock with interrupts off and publishes the new list tail last, so walkers holding only `PROC_LOCK` see a consistent list. Removal (only in `kthread_exit`) holds both `PROC_LOCK` and the spin lock.

Registration also fills two things the other files leave empty: `td_lock` (pointed at `xg_thread_lock_mtx`) and `td_proc`. `xg_sched_init()` initialises `xg_thread_lock_mtx` and proc0's mutexes when nobody has, sets up `proc0.p_threads` and puts proc0 on `allproc`. It runs from a `SYSINIT` at `SI_SUB_LOCK` and again, idempotently, from the entry points that need it.

`pfind` knows only proc0 (pid 0). `tdfind` finds registered threads and returns with `PROC_LOCK` held, like FreeBSD.

## kthreads

`kthread_add` starts an XNU thread with `kernel_thread_start`. The new thread makes its own shadow (shadows are keyed on the running XNU thread), hands it back to the creator and then waits until it may run: at once, or for `RFSTOPPED` at `sched_add`. `sched_add` drops the thread lock unless `SRQ_HOLDTD`, as FreeBSD's does, which `linux_kthread_setup_and_run` relies on. The creator's reference on the XNU thread is kept in `td_emuldata` until `kthread_exit`, so `tdsignal` can wake the thread safely.

`kthread_exit` does what FreeBSD's `thread_dtor` handler in LinuxKPI does (drops `td_lkpi_task`), unregisters the shadow, resets it for the next XNU thread that may get the same address, and calls `thread_terminate(current_thread())`, which does not return for a kernel_task thread.

`tdsignal` marks the signal pending and ends an interruptible XNU wait with `clear_wait(THREAD_INTERRUPTED)`; xg_lock's `ABORTSAFE` sleeps then return `EINTR`.

## Priorities

`sched_prio` and `rtp_to_pri` keep FreeBSD's fields (`td_priority`, `td_base_pri`, `td_user_pri`, `td_pri_class`) but do not change the XNU priority. Both callers in LinuxKPI hold the thread lock (a spin mutex with interrupts off), where `thread_policy_set` cannot be called. XNU kernel threads already run above every user thread.

## Binding and pinning

`sched_bind(td, cpu)` uses `thread_bind(cpu_to_processor(cpu))` and, when the thread is on another CPU, drops the thread lock, yields with `thread_block(NULL)` (which moves it) and takes the lock back, as ULE's `mi_switch(SWT_BIND)` does. If blocking is not allowed (another spin lock or critical section) the binding takes effect at the next switch. The bound state lives in `TDF_SCHED0` (a flag FreeBSD reserves for the scheduler) and the bound CPU in `td_oncpu`. Like ULE, bind and unbind also count in `td_pinned`.

FreeBSD's `sched_pin()` is an inline that only counts `td_pinned`, so on XNU a pinned thread can still migrate. LinuxKPI's RCU reads its per-CPU epoch record at `rcu_read_lock` and again at `rcu_read_unlock`, so a migration in between corrupts both records. `xg_sched_pin()`/`xg_sched_unpin()` bind the XNU thread to its CPU while pinned. They take effect only once a `sys/sched.h` override maps `sched_pin`/`sched_unpin` to them; that header is shared and not part of this file.

## RCU (ck_epoch)

LinuxKPI's `linux_rcu.c` is used unchanged on top of Concurrency Kit epochs. `ck_epoch_begin`/`ck_epoch_end` are inlines in `ck_epoch.h`, so the out-of-line part (`ck_epoch_init`, `ck_epoch_register`, `_ck_epoch_addref`, `_ck_epoch_delref`, `ck_epoch_synchronize_wait`) follows CK's `ck_epoch.c` (BSD-2-Clause, Samy Al Bahra) closely. On x86 (TSO) the acquire fence in `_ck_epoch_addref` is dropped, as CK does with `CK_MD_TSO`.

`linux_synchronize_rcu_cb` either binds to the CPU of a reader that blocks the grace period (`sched_bind`, above) or, on that CPU, lowers its priority and calls `mi_switch(SW_VOL | SWT_RELINQUISH)`. Since `sched_prio` does not lower the XNU priority, `mi_switch` must really let the reader run (see below).

## Group task queues

`struct gtaskqueue` is defined here: one XNU worker per queue, a raw spin lock with interrupts off, so `grouptaskqueue_enqueue` (tasklet_schedule) works from interrupt context and needs no curthread. Flags follow FreeBSD (`TASK_ENQUEUED`, `TASK_NOENQUEUE` returns `EAGAIN`). The queue is created and drained with `xg_sched_gtq_create`, `xg_sched_gtq_cancel` (EBUSY while running), `xg_sched_gtq_drain`, `xg_sched_gtq_drain_all` and `xg_sched_gtq_free`; the taskqgroup code puts the queue pointer in `grouptask.gt_taskqueue`.

## Kernel FPU sections

XNU saves FPU state only at a context switch, and during a syscall the registers hold the user thread's live state. `fpu_kern_enter` therefore enters a critical section, clears `CR0.TS` if it is set, and saves x87/SSE state with `fxsave64` into a per-CPU area; `fpu_kern_leave` restores it. That covers the SSE4.1 copy in i915 (`movntdqa`); legacy SSE leaves the upper AVX halves alone. Sections must not sleep and do not nest (LinuxKPI counts nesting itself). A `fpu_kern_ctx` is accepted but handled the same way.

## CPUID globals

`cpu_feature`, `cpu_feature2`, `cpu_stdext_feature`, `cpu_clflush_line_size`, `cpu_id` and `cpu_vendor_id` come from CPUID leaves 0, 1 and 7 as in FreeBSD's identcpu. `tsc_freq` is crystal times ratio from leaf 0x15, else the base clock from leaf 0x16, else a 10 ms measurement against `mach_absolute_time`.

## Other

`Giant` and `allproc_lock` are statically initialised with xg_lock.c's lock classes, so they work before any SYSINIT. `lkpi_alloc_current` starts as `linux_alloc_current_noop` (FreeBSD's init_main.c), and LinuxKPI's `linux_current_init` replaces it.

## Needed from other files

- `xg_curthread()` (xg_kern.c) should set `td_lock = &xg_thread_lock_mtx`, or call `xg_sched_td_register()` for each new shadow. Until then `thread_lock(curthread)` on an unregistered shadow dereferences NULL.
- `xg_kern_init()` (or `_start`) should call `xg_sched_init()` right after itself, before the first `PROC_LOCK(&proc0)` (LinuxKPI's `linux_alloc_current` takes it on the first use of `current`).
- `shim/include/sys/sched.h`: map `sched_pin()`/`sched_unpin()` to `xg_sched_pin()`/`xg_sched_unpin()`.
- `mi_switch(flags)` (misc): it is called with curthread's thread lock held and must return with it released, as on FreeBSD. For `SWT_RELINQUISH` it should block briefly (`pause` or `IOSleep(1)`) instead of `thread_block(NULL)`: a kernel thread that only yields keeps a lower-priority user thread holding an RCU read section on that CPU from running.
- `taskqgroup_attach_cpu` and friends (misc): store an `xg_sched_gtq_create()` queue in `gt_taskqueue` and use the `xg_sched_gtq_*` calls to cancel and drain.
