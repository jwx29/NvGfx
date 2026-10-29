# XeGfx HDMI B — first real i915-owned scanout allocation

Platform: Intel Meteor Lake `8086:7d55`, Lenovo `17aa:3813`.
Internal eDP pipe A remains firmware-owned. HDMI DDI B / pipe B is **off**
until the complete modeset backend, C10 PLL/PHY and separate IOGraphics
framebuffer have been implemented and verified.

## Implemented

The `xg_i915_hdmi_surface` kernel ABI is shared between XeGfx's
XNU shim and the in-kernel i915 LinuxKPI implementation:

- `gpu/xegfx/shim/include/xg_i915.h` defines the 48-byte
  `XG_HDMI_SURFACE_ABI` request with ALLOC/QUERY/RELEASE operations.
- `gpu/xkpi/src/i915/xg_i915_exec.c` owns one system-memory
  `drm_i915_gem_object`. It creates a **separate** 1920×1080×4
  (8,294,400-byte) XRGB scanout candidate. The i915 GEM allocator
  manages the backing memory; the object is pinned into the i915 GGTT
  with a 64 KiB alignment, mapped write-combined for kernel CPU access,
  cleared and flushed. It checks the GGTT offset fits the 32-bit
  `PLANE_SURF_B` register and keeps the lease pinned until release.
  Allocation failure rolls back the CPU map, VMA pin and GEM ref.
- The allocator shares i915's GGTT manager: the existing
  `xg_i915_reserve_bootfb()` protects the firmware's pipe-A GGTT
  allocation during stage 4. **There is no manual GGTT PTE
  rewriting or BAR2+offset dereference**.
- `XeGfxResident` exposes root-only `setProperties` operations to
  allocate, query and release the lease and offers a kernel-only
  `callPlatformFunction("xgHdmiSurface")` for a future
  `NMIntelFB`/HDMI framebuffer to acquire the memory. Only
  non-pointer GGTT offset, pitch, byte size and error state appear
  in IORegistry. The live kernel virtual address is **never published**.
- `XeGfxResident::stop` releases a remaining allocation.
- A portable C++ ABI test verifies layout, geometry and operations.

### Intentionally not enabled

The kernel helper **does not** assign this memory to `PLANE_SURF_B`,
enable a display pipe, program a PLL or register an IOFramebuffer. It
only creates an independent GEM/GGTT lease. This real allocation is
useful to test without risking the firmware eDP. It is not sufficient
to show an HDMI picture, and is not armed automatically on startup.

## Test (macOS)

After pulling, run the portable tests and rebuild/reinstall the
**XeGfx kernel shim and i915 LinuxKPI components**, not just NMIntelFB:

```sh
cd ~/src/NvGfx-merge-test
git pull --ff-only
bash tools/test-hdmi-portable.sh
# Build the FULL XeGfx stack using the configured existing toolchain.
bash ./build_xegfx_stack.sh --repo "$PWD" \
  --out "$HOME/src/NvGfx-merge-build" --no-patches
```

Because the in-kernel i915 bridge is new, a full stack rebuild and
careful check of compile errors are required. A portable ABI test
does **not** guarantee the local external Linux source version has
the exact same GEM/VMA API as upstream.

Compile the root-only test utility:

```sh
xcrun clang -std=c11 -Wall -Wextra -Werror \
  -framework IOKit -framework CoreFoundation \
  tools/hdmi-ggtt-surface.c -o hdmi-ggtt-surface
```

Only once the updated XeGfx kext has been built and loaded to at least
stage 4 (i915 attached and the boot surface reserved):

```sh
sudo ./hdmi-ggtt-surface allocate
sudo ./hdmi-ggtt-surface query
sudo ./hdmi-ggtt-surface release
```

**Expected on success:** a nonzero GGTT allocation size of
8,294,400 bytes, pitch 7,680 bytes, an allocator-owned GGTT offset,
and `allocated-ggtt-not-displayed`. After release, bytes and
offset must be zero. A second allocate while the first lease exists
should return `EBUSY`, not leak another object. If the local i915
shim's GEM mapping or cache operations are unsupported, the
operation must fail and unwind rather than enabling a broken display.

Do not run this before the actual i915 stage 4 or while experimenting
with display KMS toggles. Preserve the existing working EFI.

## Remaining independent engineering work

1. Verify i915's pinned scanout is CPU-writeable and **display
   cache coherent** on the exact macOS LinuxKPI port; provide a
   lifetime-safe kernel interface to a separately instantiated HDMI
   IOFramebuffer and XeGfxAccel head 5.
2. Port the actual Meteor Lake `DC_OFF`/PSR, shared power/PHY
   ownership and C10 PLL1 program/lock/unlock backend. The working
   Linux final PLL state has been recorded but must **not** be copied
   directly into sideband PHY registers without its enable sequence.
3. Implement DBUF/DDB and watermarks; validate format and cache,
   HDMI link information, pixel clock, and required DDI-before-
   transcoder enable order for this display generation.
4. Implement pipe B and plane B modesetting with rollback, frame
   counter verification and live monitor connection handling.
5. Publish/stop the real second IOFramebuffer with independent
   connection state, scanout, VBL and compositing lifecycle, then
   connect the existing staged NVAccel head 5.

Linux references:
- `drivers/gpu/drm/i915/i915_gem.h`:
  `i915_gem_object_ggtt_pin()`
- `drivers/gpu/drm/i915/gem/i915_gem_shmem.c`:
  `i915_gem_object_create_shmem()`
- `drivers/gpu/drm/i915/display/intel_cx0_phy.c`: C10 PHY
  message bus and DPLL programming
- `docs/XEGFX-HDMI-LINUX-REFERENCE-20261029.md`: user-verified
  HDMI PLL1, power domains, and working pipe B reference
