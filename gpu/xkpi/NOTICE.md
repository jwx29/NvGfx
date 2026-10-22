# gpu/xkpi

Overlay for the XeGfx build (`XEGFX_OVERLAY`, see `gpu/xegfx/harness/score.py`): replacements for drm-kmod files
that are GPL-only, so the kext contains no GPL-only code (Legion I-097), and LinuxKPI fixes not yet in FreeBSD.

| Path | Origin | Licence |
|---|---|---|
| `include/linux/dma-buf.h`, `dma-fence-unwrap.h`, `sync_file.h`, `src/dmabuf/nmx_dma_buf.c`, `nmx_dma_fence_unwrap.c` | NullMoth, nvidia-macos-driver commit bf2dee3, `gpu-port/xkpi/override/` (audited: Hackintosh `research/repository-audits/nullmoth-gpu-port.md`) | PolyForm Noncommercial 1.0.0, as the rest of this repository |
| `include/uapi/linux/sync_file.h` | Linux v6.14 | GPL-1.0+ WITH Linux-syscall-note (interface header) |
| `include/drm/drm_format_helper.h` | written for this fork | MIT |
| `exclude.txt` | written for this fork | MIT |
| `src/lkpi/linux_schedule.c` | FreeBSD `sys/compat/linuxkpi/common/src/linux_schedule.c` (freebsd-src d2145e9) with one change: `woken_wake_function` sets `TASK_WAKING` under the channel lock, so a wake between a waiter's condition check and `schedule_timeout()` is not lost (GuC replies timed out). The bug and fix were found by [VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics](https://github.com/VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics) (ROADMAP Phase 4, `override/src/linux_schedule.c`) | BSD-2-Clause, as the FreeBSD file |
| `src/i915/intel_breadcrumbs.c` | drm-kmod `drivers/gpu/drm/i915/gt/intel_breadcrumbs.c` with two changes: its local includes say `gt/` because the file builds from here, and `signal_irq_work()` reads the next signaler before the loop body can drop the last reference to the current one. That is the upstream patch "drm/i915/gt: Fix ce use-after-free in signal_irq_work()" by Wentao Liang (linux-kernel, 2026-09-16), written with FreeBSD LinuxKPI's RCU list macros; [VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics](https://github.com/VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics) hit this use-after-free on Meteor Lake | MIT, as the i915 file |
| `src/i915/xg_i915_exec.c` | written for this fork: the XeGfx stage 4 GT check and the stage 5 per-engine request | MIT |

# gpu/userland

| Path | Origin | Licence |
|---|---|---|
| `patches/mesa-macos.patch`, `patches/libdrm-macos.patch`, `tools/vkprobe.c`, `drmlist.c`, `fill.comp` | NullMoth, nvidia-macos-driver commit bf2dee3, `gpu-port/` (audited) | PolyForm Noncommercial 1.0.0 |
| `scripts/`, `BUILD.md`, `*.ini` | written for this fork | as the rest of this repository |

Upstream sources the scripts fetch: [Mesa](https://gitlab.freedesktop.org/mesa/mesa) bffd919 (MIT) and [libdrm](https://gitlab.freedesktop.org/mesa/drm) b97cbde (MIT).
