# XeGfx WindowServer publication performance — VedCloud comparison

This patch adapts **only the guarded, staging-based IOSurface publication idea**
from VedCloud's Meteor Lake NVMTL fork. It does **not** replace XeGfx/i915,
change native scanout, remove the WindowServer host-import safety workaround, or
force direct GPU access to pageable/read-only memory.

## What changed

1. `plugin/nvmtl_vk.c`: one contiguous `memcpy` for tightly packed image
   readbacks (previously one `memcpy` for each scanline). Padded layouts
   retain the original per-row copy.
2. `plugin/NVMTLObjects.m`: opt-in `NVMTL_WS_STAGED_SURFOUT=1` on **WindowServer**
   only. The existing shared-VRAM path remains first. If GPU-to-IOSurface host
   import is still available, it remains first as well. When host import is
   already unavailable or unsuitable, GPU readback can complete into bounded
   CPU staging memory **before** locking IOSurface; only the final validated
   CPU copy takes the IOSurface lock. A changed surface seed prevents publishing
   an obsolete frame; failed GPU reads never publish.
3. Three try-lock staging slots, max 16 MiB each (48 MiB total) with fallback
   to the existing path if all are busy or allocation fails.
4. `NVMTL_WS_PRESENT_TRACE=1` gives up to 64 bounded diagnostic lines reporting
   total, IOSurface lock, readback, and CPU copy time in WindowServer.

## Test on the Meteor Lake laptop

Build and install the updated plugin from this branch using your existing
`build_xegfx_stack.sh` and `install.sh`. Before starting WindowServer, use
the existing NVMTL config file to enable the diagnostics without changing
global boot arguments:

```sh
sudo cp /Library/GPUBundles/nvmtl/nvrm610.conf /Library/GPUBundles/nvmtl/nvrm610.conf.before-perf
echo 'NVMTL_WS_PRESENT_TRACE=1' | sudo tee -a /Library/GPUBundles/nvmtl/nvrm610.conf
```

Restart WindowServer and drag a window. Locate `ws-present` entries in the
NVMTL log (same destination as `nvlog`). Notice whether time is spent on
`gpu-or-read` or `lock`, and whether the original copy path is used.

Next, enable the staged fallback via the same config file:

```sh
printf 'NVMTL_WS_STAGED_SURFOUT=1\\n' | sudo tee -a /Library/GPUBundles/nvmtl/nvrm610.conf
```

Restart WindowServer again to load the updated process-local configuration.
Compare responsiveness, artifacts, GPU/readback times and CoreAnimation timeouts.
The staged path is intentionally disabled by default until you confirm it is
stable on the actual 8086:7D55 device.

**Rollback:** remove the two `NVMTL_WS_...` lines from the config file (or
restore the copied file) and restart WindowServer. Reboot into your fallback EFI
if the desktop hangs. Do not enable this permanently until tested under load.

## Correctness checks

```sh
cc -std=c11 -Wall -Wextra -Werror -pthread tools/test_nvmtl_ws_stage.c -o /tmp/test_nvmtl_ws_stage
/tmp/test_nvmtl_ws_stage
```

Tests cover 1080p geometry, overflow/size rejection, bounded pool contention,
padded and contiguous copies, and destination bounds. They do not simulate
physical Intel scanout or Vulkan synchronization.

Source references:
- https://github.com/VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics/blob/main/NullMoth/plugin/nvmtl_surface_publication.h
- https://github.com/VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics/blob/main/research/scanout-copy-0.3.19.md
