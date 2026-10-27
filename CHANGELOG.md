# Changelog

Versions are the kext's `CFBundleVersion`. "Booted" means the version ran on the test laptop (Legion 5 15IAX10, Arrow Lake iGPU `8086:7D67`); "built" means it compiled and has not been run. Prereleases with a binary: v0.0.27, v0.0.35, v0.0.36, v0.0.38 and v0.0.45.

## 0.0.45 (booted: N-295, N-296)

N-295 (`gtawake=2`) froze 16 minutes into the Intel desktop with the same rendezvous panic, so forcewake is not the I-116 cause. N-296 ran the same build with `xegfx.nvram_trace=0` and the Intel desktop stayed up for the full hour. The trace's NVRAM writes run in SMM on this firmware, where a CPU cannot answer the rendezvous interrupt, so the trace is now the main suspect. Leave it off for desktop runs.


- `xegfx.gtawake=1` holds every GT's wakeref, `=2` also takes forcewake on all domains, so the GT never enters RC6. Off by default. It mirrors what stopped the whole-Mac freezes on another Xe port (I-116). The implementation of `xg_i915_gtawake` is in the NvGfx fork's `gpu/xkpi` overlay.
- `XeGfxGt` in the registry reports `gtawake-gts`.

## 0.0.44 (built, not booted)

- `xegfx.irqoff_check=0` turns off the interrupts-off checker and its 20 ms wakeup, to A/B it against the I-116 freeze. The checker stays on by default.

## 0.0.43 (booted: N-290)

- The interrupts-off watchdog names the CPU and the caller before macOS hits its rendezvous timeout (I-116). On N-290 the Intel desktop stayed up for the 900 s test, the first run to do so since I-116 appeared.

## 0.0.42 (booted: N-289, crashed at about 15 to 20 s with the I-116 rendezvous panic)

- Reverts the blanket rwlock spin from 0.0.40, which caused rendezvous panics (N-287, N-288).
- `xg_map_mmap` takes the lookup lock before `rcu_read_lock` instead, which fixes the sleeping-lock-in-RCU panic on WindowServer's mmap path.

## 0.0.41

- Interrupt and handler counts in `debug.xegfx_spin`, for the N-287 stall.
- XeGfxIntelFB 0.5 names the iGPU so About This Mac shows "Intel Xe Graphics".

## 0.0.40

- rwlocks spin inside critical sections, after WindowServer's mmap-offset lookup blocked under `rcu_read_lock` and panicked (N-286). Reverted in 0.0.42.

## 0.0.39

- Logs why a userptr hold fails (WindowServer execbuffer EFAULT, N-283).
- XeGfxIntelFB 0.3 clears the guard with a zero count.

## 0.0.38 and earlier

See the git history. 0.0.38 rebases syncobj wait deadlines from Darwin's CLOCK_MONOTONIC onto the shim's ktime; 0.0.37 returns `ETIME` for LinuxKPI's `ETIMEDOUT` so Mesa sees `VK_TIMEOUT`.
