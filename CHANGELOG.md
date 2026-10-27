# Changelog

Versions are the kext's `CFBundleVersion`. "Booted" means the version ran on the test laptop (Legion 5 15IAX10, Arrow Lake iGPU `8086:7D67`); "built" means it compiled and has not been run. Only the v0.0.27 prerelease has a published binary.

## 0.0.45 (built, not booted)

- `xegfx.gtawake=1` holds every GT's wakeref, `=2` also takes forcewake on all domains, so the GT never enters RC6. Off by default. It mirrors what stopped the whole-Mac freezes on another Xe port (I-116). The implementation of `xg_i915_gtawake` is in the NvGfx fork's `gpu/xkpi` overlay.
- `XeGfxGt` in the registry reports `gtawake-gts`.

## 0.0.44 (built, not booted)

- `xegfx.irqoff_check=0` turns off the interrupts-off checker and its 20 ms wakeup, to A/B it against the I-116 freeze. The checker stays on by default.

## 0.0.43 (booted: N-290)

- The interrupts-off watchdog names the CPU and the caller before macOS hits its rendezvous timeout (I-116). On N-290 the Intel desktop stayed up for the 900 s test, the first run to do so since I-116 appeared.

## 0.0.42 (booted: N-289)

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
