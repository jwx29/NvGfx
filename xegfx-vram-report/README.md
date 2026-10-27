# XeGfx shared-memory reporting (8086:7d55, macOS 15)

This is a reporting-only adaptation of VedCloud's dynamically queried ANV
shared-memory reporter for jwx29/NvGfx `meteor-lake-local`.

## Important

- Review all code before use. The helper and kext patch have **not** been
  compiled on the user's macOS system and are not hardware validated.
- Do not patch `getVRAMRange`, `getApertureRange`, `IOFBMemorySize`,
  scanout mapping, or Mesa's memory allocation limits.
- Install only after preserving the working GPU/EFI backup and kernel panic logs.
- The helper needs the Intel Vulkan ICD initialized. With this repo's manual
  bring-up workflow, call the reporter AFTER a successful `boot-gpu.sh`.
- The registry reporting property is transient; it must be republished each
  boot after the GPU is available. Do not create a startup daemon while
  investigating kernel panics.

## 1. Patch source

    cd ~/src/NvGfx
    cp kexts/NMIntelFB/intelfb.cpp kexts/NMIntelFB/intelfb.cpp.vram-before
    patch --dry-run -p1 < /path/to/nmintelfb-vram-report.patch
    patch -p1 < /path/to/nmintelfb-vram-report.patch
    git diff --check
    git diff -- kexts/NMIntelFB/intelfb.cpp

## 2. Build helper in this folder

    xcrun clang -arch x86_64 -fobjc-arc -mmacosx-version-min=15.0 \
      -I"$HOME/src/deps/Vulkan-Headers/include" \
      xegfx-gpu-report.m -o xegfx-gpu-report \
      -framework Foundation -framework IOKit
    codesign -f -s - xegfx-gpu-report

## 3. Read-only query first (GPU must be initialized)

    ./xegfx-gpu-report --query-only

Expected: `sharedHeapBytes: 12884901888`, **not** dynamic heapBudget.

## 4. Build framebuffer and install (after recovery backup)

    cd ~/src/NvGfx
    bash build/nvgfx/build_nmintelfb.sh
    bash ./install.sh --check
    bash ./install.sh

`install.sh` does a full-stack installation, including KC rebuild; it requires
existing staged builds and creates backup folders under `/Library/XeGfx/`.
Reboot and only activate manually once the boot is stable.

## 5. Publish AFTER GPU activation, then verify

    sudo /path/to/xegfx-gpu-report
    ioreg -l -w0 | grep -E 'VRAM,totalMB|XeGfxSharedHeapBytes|IOFBMemorySize'
    system_profiler SPDisplaysDataType

Expected PCI property: `VRAM,totalMB = 12288`; framebuffer `IOFBMemorySize`
remains ~9,216,000 bytes. If the property readback verifies but System
Information still shows the small scanout number, inspect the full PCI
registry tree and `system_profiler` JSON before changing any memory mappings.

## 6. Rollback

    cd ~/src/NvGfx
    patch -R -p1 < /path/to/nmintelfb-vram-report.patch

Restore a previously built/installed NMIntelFB from your known-good recovery
backup if the new kernel build malfunctions. Reverting source does NOT revert
an installed kext; rebuild/reinstall or restore backup and reboot.
