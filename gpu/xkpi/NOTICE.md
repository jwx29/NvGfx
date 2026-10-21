# gpu/xkpi

Overlay for the XeGfx build (`XEGFX_OVERLAY`, see `gpu/xegfx/harness/score.py`): replacements for drm-kmod files
that are GPL-only, so the kext contains no GPL-only code (Legion I-097).

| Path | Origin | Licence |
|---|---|---|
| `include/linux/dma-buf.h`, `dma-fence-unwrap.h`, `sync_file.h`, `src/dmabuf/nmx_dma_buf.c`, `nmx_dma_fence_unwrap.c` | NullMoth, nvidia-macos-driver commit bf2dee3, `gpu-port/xkpi/override/` (audited: Hackintosh `research/repository-audits/nullmoth-gpu-port.md`) | PolyForm Noncommercial 1.0.0, as the rest of this repository |
| `include/uapi/linux/sync_file.h` | Linux v6.14 | GPL-1.0+ WITH Linux-syscall-note (interface header) |
| `include/drm/drm_format_helper.h` | written for this fork | MIT |
| `exclude.txt` | written for this fork | MIT |

# gpu/userland

| Path | Origin | Licence |
|---|---|---|
| `patches/mesa-macos.patch`, `patches/libdrm-macos.patch`, `tools/vkprobe.c`, `drmlist.c`, `fill.comp` | NullMoth, nvidia-macos-driver commit bf2dee3, `gpu-port/` (audited) | PolyForm Noncommercial 1.0.0 |
| `scripts/`, `BUILD.md`, `*.ini` | written for this fork | as the rest of this repository |

Upstream sources the scripts fetch: [Mesa](https://gitlab.freedesktop.org/mesa/mesa) bffd919 (MIT) and [libdrm](https://gitlab.freedesktop.org/mesa/drm) b97cbde (MIT).
