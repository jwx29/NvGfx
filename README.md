# NvGfx — Meteor Lake Xe-LPG on macOS Sequoia

> **Experimental, machine-specific Hackintosh GPU enablement.** This branch is not the upstream NVIDIA driver and is not a general-purpose Intel graphics release. It combines the upstream XeGfx FreeBSD/LinuxKPI-to-XNU port, Mesa ANV, a Metal-on-Vulkan plugin, and custom framebuffer/AGDC integration for one Intel Meteor Lake system.

**Repository:** [`jwx29/NvGfx`](https://github.com/jwx29/NvGfx)  
**Branch:** [`meteor-lake-local`](https://github.com/jwx29/NvGfx/tree/meteor-lake-local)  
**Base:** [`watchthelight-labs/NvGfx`, branch `xe`](https://github.com/watchthelight-labs/NvGfx/tree/xe)  
**Comparison:** [upstream `xe` → `meteor-lake-local`](https://github.com/jwx29/NvGfx/compare/xe...meteor-lake-local)  
**Documented snapshot:** 27 October 2026. The branch was seven commits ahead of the matching upstream `xe` commit (`5a19a73f36249efb99c44c3f9cb34603ef0c0eb8`) and zero behind when this README was prepared.

## Contents

1. [Scope and current status](#scope-and-current-status)
2. [How the stack fits together](#how-the-stack-fits-together)
3. [Hardware and software requirements](#hardware-and-software-requirements)
4. [Directory layout](#directory-layout)
5. [First-time setup](#first-time-setup)
6. [Build the stack](#build-the-stack)
7. [Install the stack](#install-the-stack)
8. [Activate the GPU after boot](#activate-the-gpu-after-boot)
9. [Run tests and inspect diagnostics](#run-tests-and-inspect-diagnostics)
10. [Changes relative to upstream `xe`](#changes-relative-to-upstream-xe)
11. [Firmware memory panic and LegionCbmrFix](#firmware-memory-panic-and-legioncbmrfix)
12. [Known issues, experiments, and limitations](#known-issues-experiments-and-limitations)
13. [Rollback and recovery](#rollback-and-recovery)
14. [Maintaining the fork](#maintaining-the-fork)
15. [Credits and licensing](#credits-and-licensing)

## Scope and current status

This work targets an **Intel Core Ultra 7 155H (Meteor Lake)** integrated GPU, PCI **`8086:7D55`**, revision **`0x08`**, running **macOS Sequoia 15.x**, tested in the project on macOS 15.8.1 / Darwin 24.6.0. The internal panel has been observed at **1920 × 1200**. The intended environment is an **x86_64 OpenCore Hackintosh**; this is not an Apple-supported configuration.

The development goal is to expose a working Intel DRM render node and use **Mesa ANV → Vulkan → NVMTLDriver → Metal/WindowServer**, while displaying the resulting desktop through the existing firmware-programmed Intel panel. The display path remains experimental and uses elements adapted from the upstream NvGfx/NVRM accelerator implementation.

### Observed progress (not a stability guarantee)

- XeGfx kernel bring-up **stages 1–7** have passed in local tests, including GPU command execution and writeback.
- The Intel GPU is registered in WindowServer through **`XeGfxDeviceControl`**, **`NMXAccel`**, and **`NMIntelFB`**.
- A DRM render node, `/dev/dri/renderD128`, has been usable for XeGfx and Mesa testing.
- Metal is reported as supported, and the reported presentation test completed **301/301** operations in an earlier test session. These results do not prove all Metal features or applications work.
- A serious XNU panic, `vm_page_validate_no_references`, was reproduced after accelerated WindowServer use. An upstream UEFI workaround, **LegionCbmrFix**, has since been confirmed to unload `cbmr_driver.efi` and reserve its affected physical range on this machine. **Long-term post-workaround stability was not established at the time of writing.**

**Do not treat successful stages, successful self-tests, or a detected Metal device as proof that WindowServer will remain stable.** Keep an independent bootable recovery path and known-good OpenCore configuration.

## How the stack fits together

```text
OpenCore + firmware-programmed Intel panel
  |
  +-- NMIntelFB.kext       (physical display / existing scanout)
  |     +-- IOFramebuffer registration, dependent index 0
  |     +-- scanout mapping and display-mode metadata
  |
  +-- XeGfx.kext          (FreeBSD/LinuxKPI compatibility + Intel i915/GuC)
  |     +-- DRM render node (/dev/dri/renderD128)
  |     +-- staged activation, sysctls, diagnostic log
  |
  +-- XeGfxAccel.kext     (NMXAccel / IOAccelerator integration)
  |     +-- Intel display is accelerator head 4 (bit 0x10)
  |     +-- IOAccel display-pipe transactions
  |
  +-- XeGfxAGDC.kext      (AppleGraphicsDeviceControl for the Intel GPU)
  |     +-- framebuffer slot 0 and display capability replies
  |
  +-- /Library/GPUBundles/
        +-- NVMTLDriver.bundle        (Metal plugin)
        +-- nvmtl/libvulkan_intel.dylib (Mesa ANV)
        +-- nvmtl/libdrm.2.dylib
        +-- nvmtl/intel_icd.json
```

**Do not confuse these indices:** `IOFBDependentIndex = 0` means the framebuffer occupies **IOGraphics framebuffer slot 0**; `NMXAccel` **head 4** is the accelerator's own internal head number, represented by bit `16` (`0x10`). The fork intentionally needs both.

The upstream `xe` branch supplies the majority of the port: XeGfx, the LinuxKPI adaptation, i915/GuC bring-up, DRM mapping, the accelerator framework, the base Intel framebuffer, AGDC support, Metal plugin, and tests. This branch mostly adds **integration scripts** and adjusts how these existing components connect on the target Sequoia installation.

## Hardware and software requirements

### Runtime

- Intel Meteor Lake graphics, specifically verified against **`8086:7D55`** in the project.
- A working OpenCore/Sequoia 15.x boot configuration and a fallback boot option.
- Existing functional panel output **before** attempting accelerated WindowServer activation.
- All four kexts built for **x86_64 macOS 15** where applicable: `XeGfx`, `XeGfxAccel`, `NMIntelFB`, and `XeGfxAGDC`.
- An installed `NVMTLDriver.bundle`, Mesa ANV, and libdrm.

### Build prerequisites used by `build_xegfx_stack.sh`

The wrapper **does not download or compile all third-party dependencies**. It assumes the upstream toolchains and source trees have already been set up. Defaults are:

| Input | Default location / source |
|---|---|
| NvGfx repository | `~/src/NvGfx` |
| Build outputs | `~/src/NvGfx-build` |
| Intel/FreeBSD/Linux source checkout | `~/src/xe-src` |
| Intel firmware | `~/src/xe-src/linux-firmware`, including `mtl_guc_70.bin` and `LICENSE.i915` |
| NVIDIA open GPU modules dependency used by accelerator build | `~/src/deps/open-gpu-610.57.04` (`OGKM`) |
| Vulkan headers | `~/src/deps/Vulkan-Headers/include/vulkan` |
| XeGfx ABI verification tool | `~/src/kcabi/linkcheck.py` |
| DRM userspace headers | `~/src/xe-userland/drm/include/drm` |
| Built Mesa/libdrm libraries for installation | `~/src/xe-userland/prefix/lib` |
| macOS SDK | `MacOSX15.5.sdk` if installed; otherwise current `xcrun` macOS SDK |

Required commands checked by the wrapper include `git`, `xcrun`, `python3`, `codesign`, `clang`, `shasum`, `tee`, `ditto`, and `tar`. Some optional build stages need additional tooling, such as `cargo` for the translator. A **full libdrm/Mesa build is not included**: the wrapper can call an explicitly supplied existing command, but it cannot invent the user's userspace build procedure.

If your paths differ, use `--repo`, `--out`, or the documented environment variables; do **not** copy commands from an unrelated checkout without checking them first.

## Directory layout

```text
NvGfx/
├── boot-gpu.sh                    # Per-boot staged GPU activation and tests
├── build.sh                       # Old local wrapper; see warning below
├── build_xegfx_stack.sh           # Main non-installing stack build wrapper
├── install.sh                     # Sequoia 15 full-stack installer
├── verify-plugins.sh             # Installed plugin/kext inspection
├── build/nvgfx/                  # Upstream component build scripts
│   ├── build_xegfx.sh
│   ├── build_nmintelfb.sh
│   ├── build_xegfxagdc.sh
│   ├── build_nvaccel.sh           # Locally changed to build XeGfxAccel for 15
│   ├── build_plugin.sh
│   └── build_xlate.sh
├── gpu/xegfx/                   # Upstream kernel shim, i915, tools, tests
│   ├── tools/install.sh          # Installer wrapped by root install.sh
│   └── out/                      # XeGfx build outputs
├── gpu/userland/scripts/env.sh   # Uses current xcrun-selected SDK
├── kexts/NMIntelFB/intelfb.cpp   # Panel grouping, timing, scanout ABI changes
├── kexts/NVRMAGDC/nvrm-agdc.cpp  # Intel matching and AGDC capability changes
└── plugin/NVMTLDevice_contract.m # Committed WindowServer host-copy guard
```

**Note:** `build/nmintelfb-output/` currently contains checked-in compiled/generated artifacts, including a kext executable and an object file. Those are **not** the recommended authoritative build inputs or the output locations used by the new wrapper. Consider removing generated outputs from source control in a separate housekeeping commit after verifying that no workflow depends on them.

## First-time setup

### 1. Get the correct branch

```bash
mkdir -p ~/src
cd ~/src
git clone -b meteor-lake-local https://github.com/jwx29/NvGfx.git NvGfx
cd NvGfx
```

If you already have the checkout, preserve local modifications before pulling. To track upstream independently:

```bash
git remote -v
git remote add upstream https://github.com/watchthelight-labs/NvGfx.git  # only if missing
git fetch upstream xe
```

### 2. Validate the configured build environment

```bash
cd ~/src/NvGfx
bash ./build_xegfx_stack.sh --check
```

`--check` performs dependency/path checks only; it does **not** patch the checkout, build, install, or restart anything. Fix missing paths or tools before proceeding. On macOS use the SDK reported by `xcrun --sdk macosx --show-sdk-path` if the specific 15.5 SDK is unavailable, but a newer SDK may affect compatibility and must be tested.

### 3. Check userspace dependencies

The installer expects these existing files by default:

```bash
ls -l ~/src/xe-userland/prefix/lib/libdrm.2.dylib
ls -l ~/src/xe-userland/prefix/lib/libvulkan_intel.dylib
ls -l /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver
```

The repository's `gpu/userland/scripts/env.sh` now selects the SDK using `xcrun`; it does **not** itself build Mesa or libdrm. Follow the upstream XeGfx userspace build instructions for those dependencies.

## Build the stack

The preferred entry point is **`build_xegfx_stack.sh`**.

### Normal build (no extra WindowServer copy patch)

```bash
cd ~/src/NvGfx
bash ./build_xegfx_stack.sh \
  --repo "$HOME/src/NvGfx" \
  --out "$HOME/src/NvGfx-build" \
  --upstream-ref upstream/xe
```

The wrapper does the following, in order:

1. Checks prerequisites and file locations.
2. Creates a uniquely named run under `~/src/NvGfx-build/runs/<timestamp>-<pid>/`.
3. Archives Git status, committed differences from upstream, unstaged/staged differences, and any untracked source files.
4. Applies the three recognised, idempotent macOS 15 integration fixes described below (unless `--no-patches` is supplied).
5. Builds **NMIntelFB → XeGfxAGDC → XeGfxAccel → XeGfx → NVMTLDriver**.
6. Ad-hoc signs **copied build artifacts** and bundles them into `artifacts.tar.gz`; does not install to `/Library`.
7. Saves a manifest, per-component logs, source patch inventory, and SHA-256 hashes.

Successful output is under:

```text
~/src/NvGfx-build/runs/<timestamp>-<pid>/
├── MANIFEST.txt
├── PATCHES.md
├── logs/
├── patches/
│   ├── before/
│   ├── after-patching/
│   ├── finished/
│   └── originals/          # only files changed by the wrapper
├── artifacts/
│   ├── NMIntelFB.kext
│   ├── XeGfxAGDC.kext
│   ├── XeGfxAccel.kext
│   ├── XeGfx.kext
│   ├── NVMTLDriver
│   └── SHA256SUMS.txt
└── artifacts.tar.gz
```

The individual component outputs remain under `~/src/NvGfx-build/kexts/`, `~/src/NvGfx-build/plugin/`, and `~/src/NvGfx/gpu/xegfx/out/`.

### Build options

| Option | Effect |
|---|---|
| `--check` | Dependency preflight only; no patch/build/install |
| `--repo PATH` | Override the checkout |
| `--out PATH` | Override output root |
| `--upstream-ref REF` | Select the local Git ref for exported patch history; **no fetch is performed** |
| `--no-patches` | Skip the wrapper's three idempotent source edits; committed changes remain |
| `--no-plugin` | Skip `NVMTLDriver` |
| `--with-xlate` | Also build `libnvmtl_translate.dylib`; may need `NVGFX_REF_PKGROOT` |
| `--with-userland` | Invoke `XEGFX_USERLAND_CMD`, your own Mesa/libdrm build command |
| `--ws-copy` | Apply an **experimental** WindowServer host-buffer copy guard to `NVMTLDevice_contract.m` |
| `--help` | Print usage |

Examples:

```bash
# Build without altering source from the committed branch:
bash ./build_xegfx_stack.sh --no-patches

# Build the kernel-side pieces, without the Metal plugin:
bash ./build_xegfx_stack.sh --no-plugin

# Translator (requires its additional dependencies/reference package):
NVGFX_REF_PKGROOT=/path/to/reference/package/root \
  bash ./build_xegfx_stack.sh --with-xlate

# Userspace: run a command YOU have already validated:
XEGFX_USERLAND_CMD='bash /absolute/path/to/my-mesa-build.sh' \
  bash ./build_xegfx_stack.sh --with-userland
```

Environment defaults/overrides: `NVGFX_REPO`, `NVGFX_OUT`, `XEGFX_SOURCE`, `OGKM`, `XEGFX_UPSTREAM_REF`, `NVGFX_DEPS`, `SDKROOT`, `OUTROOT`, `XEGFX_FW`, `XEGFX_DRM_INC`, `NVGFX_REF_PKGROOT`, and `XEGFX_USERLAND_CMD`.

#### What the default patching step changes

The wrapper applies these exact-match corrections and refuses to guess if upstream source has materially changed:

| File | Applied correction | Reason |
|---|---|---|
| `kexts/NMIntelFB/intelfb.cpp` | Set two `IOFBDependentIndex` declarations from `4` to `0` if necessary | IOGraphics/AGDC needs the panel in framebuffer slot **0**; accelerator head 4 is separate |
| `kexts/NVRMAGDC/nvrm-agdc.cpp` | Use `AGDC_FB_CLASS "NMIntelFB"` in the XeGfx build | The prior `IONDRVFramebuffer` match could miss the actual claimed panel |
| `build/nvgfx/build_nvaccel.sh` | Build `XeGfxAccel` for major version **15** instead of 26 | Sequoia needs its own accelerator kext build; expected path `XeGfxAccel/15/` |

**All three corrections are already present in the pushed `meteor-lake-local` sources.** Accordingly, default builds should report them as already applied/unchanged. `--no-patches` does not reverse committed edits.

#### Important: `--ws-copy` and the committed guard

`--ws-copy` inserts an `XEGFX_WS_HOST_COPY` guard into `plugin/NVMTLDevice_contract.m` only if the marker is absent. **The pushed branch already contains this guard**, so even a normal build **without** `--ws-copy` still forces WindowServer's `newBufferWithBytesNoCopy` *normal import path* through the copy fallback. Its separate managed-buffer/shadow branch remains as in the source.

The guard was a diagnostic experiment during a memory-panic investigation. It is **not an established permanent fix** and may have a performance cost. To compare with the upstream code, remove the specific `XEGFX_WS_HOST_COPY` hunk in a separate, reviewable change and rebuild the plugin. Merely omitting `--ws-copy` does not remove existing code.

#### `build.sh` is a stale convenience wrapper

The committed root `build.sh` currently runs:

```bash
bash ~/Downloads/build_xegfx_stack.sh --check
bash ~/Downloads/build_xegfx_stack.sh \
  --repo "$HOME/src/NvGfx" --out "$HOME/src/NvGfx-build" \
  --upstream-ref origin/xe --ws-copy
```

It **does not invoke the copy inside the repository** and assumes the script also lives in `~/Downloads`. It also opts into `--ws-copy` and uses `origin/xe` (which might not exist if `origin` is your fork). Prefer `bash ./build_xegfx_stack.sh` as shown above; update or remove this wrapper in a future cleanup commit.

## Install the stack

**`install.sh` is an installer, not a build script.** It modifies `/Library/Extensions`, `/Library/GPUBundles`, and the Auxiliary Kernel Collection. Have a recovery USB/EFI backup before running it.

The script is intended for **x86_64 macOS Sequoia 15.x only** and refuses other macOS major versions. It expects an **existing `NVMTLDriver.bundle`** that can be copied and re-signed; it does not create a complete bundle from scratch.

### Preflight

```bash
cd ~/src/NvGfx
bash ./install.sh --check
```

It elevates using `sudo` as required but does not change installed files in `--check` mode. It checks the four kext bundle identifiers, the corrected AGDC's `NMIntelFB` reference, the plugin binary, and the ANV/libdrm libraries.

To override defaults:

```bash
NVGFX_REPO="$HOME/src/NvGfx" \
NVGFX_OUT="$HOME/src/NvGfx-build" \
XEGFX_USERLAND="$HOME/src/xe-userland/prefix/lib" \
  bash ./install.sh --check
```

### Installation

```bash
cd ~/src/NvGfx
bash ./install.sh
```

The installer:

1. Stages a **copy** of the existing Metal bundle with the newly built `NVMTLDriver` executable; ad-hoc signs and verifies it.
2. Runs upstream `gpu/xegfx/tools/install.sh` with **explicit** paths for all four kexts and both userland libraries; rebuilds the Auxiliary Kernel Collection.
3. Moves the original Metal bundle into a dated backup directory, then swaps in the signed new bundle.
4. Checks installed signatures, expected libraries, kext files, and installed plugin hash.
5. Prints the backup path and asks for a **manual reboot**; it does not reboot itself.

Backups are saved under:

```text
/Library/XeGfx/full-install-YYYYmmdd-HHMMSS-PID/
```

The upstream installer also keeps its own backup under `/Library/XeGfx/backup-.../`. If installation fails partway through, inspect both backups and `upstream-install.log` before proceeding; changes made by upstream may already have taken effect.

**Reboot before trying to load/restart XeGfx or WindowServer.** Do not replace an in-use kext or Mach-O executable in place.

### Verify installed files

```bash
cd ~/src/NvGfx
bash ./verify-plugins.sh
```

This convenience script prints:

- Matching kernel extensions reported by `kmutil showloaded`.
- The installed `/Library/GPUBundles/NVMTLDriver.bundle` executable and SHA-256.
- Bundle code-signing verification and signature details.
- Presence of `libvulkan_intel.dylib`, `libdrm.2.dylib`, and `intel_icd.json`.
- `otool -L` dependency list for the Metal plugin.

It is an inspection aid, not a comprehensive pass/fail test. Kexts deliberately loaded on demand may not appear in `kmutil showloaded` before bring-up. The script does not include `set -e`, so assess its output rather than its exit status alone.

## Activate the GPU after boot

**`boot-gpu.sh` is the per-boot bring-up script.** It uses already-installed kexts, runs kernel stages and optional userspace tests, but **does not install/rebuild anything or restart WindowServer**.

```bash
cd ~/src/NvGfx
sudo bash ./boot-gpu.sh
```

Useful alternatives:

```bash
# Inspect current state; does not activate stages:
bash ./boot-gpu.sh --status

# Activate stages, but leave display-pipe transactions disabled:
sudo bash ./boot-gpu.sh --no-iop

# Bring up the GPU without lengthy userspace tests:
sudo bash ./boot-gpu.sh --no-tests

# Run diagnostics on an already activated stack:
sudo bash ./boot-gpu.sh --tests-only

# Run the full upstream Metal suite (potentially much longer):
sudo bash ./boot-gpu.sh --tests-only --full-tests
```

`--status` still writes a dated status log to `/Users/Shared`; it does not change GPU state. `--no-iop` skips enabling `debug.nvaccel_iop=1`, but the script still configures the framebuffer registration/scanout gate.

### What the bring-up script does

| Step | Action | Why |
|---|---|---|
| 0 | Confirm `NMIntelFB` is loaded/registered | Refuse GPU activation without a working panel |
| 0a | Clear `nmintelfb-starts` **after** the working framebuffer is confirmed | Prevent past experimental WindowServer failures from exhausting the framebuffer's boot-failure guard |
| 1 | Load `org.watchthelight.XeGfx` on demand | Ensure the stage interface is actually registered |
| 2 | Set `debug.xegfx_wdog=60` | Bound stalled kernel stages |
| 3 | Run stages **1–6**, validating the separate results array after each | Test shim, PCI/MMIO, newbus/DMA, i915/GuC, execution, and writeback |
| 4 | Load `XeGfxAccel`, run **stage 7** | Claim the Intel GPU and match the accelerator PCI personality |
| 5 | Wait for accelerator **head 4**, mask bit `0x10` | Ensure the internal panel is advertised by the accelerator |
| 6 | Write `debug.nvaccelfb=4` and wait for registration/piping | Enable scanout and aperture gates; reading the sysctl normally shows armed state, not the last value written |
| 7 | Load/activate `XeGfxAGDC`; verify `debug.xegfx_agdc=1` and `fbmap='n=1 [0]...'` | Establish the GPU's AGDC service and framebuffer index 0 |
| 8 | Enable `debug.nvaccel_iop=1`, unless `--no-iop` | Allow experimental display-pipe transactions |
| 9 | Run xgprobe and Metal tests, unless `--no-tests` | Validate render-node and Metal userspace paths before manually restarting WindowServer |
| End | Write `debug.xegfx_agdc_k5=1` and print manual handoff command | Enable the experimental K5 AGDC replies and leave final WindowServer restart to the operator |

The final K5 sysctl write currently runs unconditionally and its result is not used as a final pass/fail gate; this is implementation behaviour, not a claim that all K5 replies work.

**Check the output for failed or skipped tests before restarting WindowServer.** The script intentionally distinguishes **PASS**, **FAIL/TIMEOUT**, and **SKIP**. Missing tooling is incomplete coverage, not proof of success.

### Optional: manually restart WindowServer

Only after the screen is working, stage checks pass, installed plugin/userland are verified, and you have a recovery option:

```bash
sudo killall -TERM WindowServer
```

This logs out the current graphical session and causes macOS to attach the new accelerator/display pipe. **Save all work first.** An incomplete driver may produce a black screen, hang, or kernel panic. If possible, have SSH or another way to collect logs/reboot. Do not automate this step during development.

The upstream driver activation is deliberately **per boot**. A reboot resets the manually started XeGfx runtime, although installed kexts and any OpenCore driver remain on disk.

## Run tests and inspect diagnostics

### Log files

| Origin | Location |
|---|---|
| Bring-up/status session | `/Users/Shared/xe-bringup-YYYYmmdd-HHMMSS.log` |
| Self-test results | `/Users/Shared/xe-tests-YYYYmmdd-HHMMSS-PID/` |
| Build run | `~/src/NvGfx-build/runs/<timestamp>-<pid>/logs/` |
| Installer backups/logs | `/Library/XeGfx/full-install-.../` |
| XeGfx kernel ring buffer | `sysctl -b debug.xegfx_log` |
| macOS kernel panic reports | `/Library/Logs/DiagnosticReports/*.panic` or matching kernel `.ips` files |

### Verify staged bring-up

```bash
sysctl debug.xegfx_stage
sysctl -b debug.xegfx_results | od -An -td4
sysctl debug.xegfx_agdc
sysctl debug.xegfx_agdc_fbmap
sysctl debug.nvaccel_heads_published
sysctl debug.nvaccel_heads_registered
sysctl debug.nvaccel_heads_piped
```

Expected indicators after normal bring-up include stage `7`, stage results `1` for indices 1–7, AGDC state `1`, slot map beginning `n=1 [0]`, and head-4 bit `16` present in all three accelerator head masks.

### Verify actual display activity

```bash
sysctl debug.nvaccel_pipe_perform
sysctl debug.nvaccel_iop_ok_h4
sysctl debug.nvaccel_iop_fail
sysctl debug.nvaccel_iop_src_res
sysctl debug.nvaccel_iop_src_surf
sysctl debug.nvaccel_iop_flips
```

`nvaccel_pipe_perform` and `nvaccel_iop_ok_h4` increasing mean transactions are reaching the pipeline/panel. A zero failure count is desirable, but does not establish memory safety. `src_res` and `src_surf` distinguish resource versus IOSurface presentation sources; flips are tracked separately.

### xgprobe and Metal tests

`boot-gpu.sh` uses `/usr/local/bin/xgprobe` or the checkout's built probe and defaults to `/dev/dri/renderD128`. `XEGFX_RENDER_NODE` can override that path. Its `xgprobe` watchdog is **45 seconds**.

For Metal, the script runs `tools/metal_selftest.sh` under the **logged-in, non-root desktop user**. The smoke subset requests `pipeline-build`, `surface-wrappers`, and `surface-order queued`; `--full-tests` runs the complete suite. The script caps individual test time at 120 seconds and total wall-time at 480 seconds (smoke) or 2,400 seconds (full). Exact test availability depends on the upstream checkout.

Run tests without repeating the entire bring-up:

```bash
sudo bash ./boot-gpu.sh --tests-only
```

You can also inspect the macOS unified log:

```bash
sudo log show --last 20m --style compact \
  --predicate 'process == "kernel" AND eventMessage CONTAINS[c] "xegfx"' \
  | tail -100
```

The warning `Wall Clock adjustment detected` is a log time-window diagnostic, not itself a GPU error. Repeated DRM ioctl failures may be feature probes or rejected requests; debug actual failed submissions separately rather than assuming all nonzero ioctls are kernel corruption.

## Changes relative to upstream `xe`

The comparison with upstream was checked against its **actual matching `xe` commit**, not its unrelated default NVIDIA branch. The fork was **7 commits ahead, 0 behind**, with the following added/modified paths. Broad XeGfx, ANV, LinuxKPI, and NVMTL implementation files are inherited rather than newly written in this branch.

### Source and integration changes

| Path | Actual branch change | Motivation / status |
|---|---|---|
| `kexts/NMIntelFB/intelfb.cpp` | Publish `IOFBDependentID` and dependent index **0 before** `IOFramebuffer::start()` in both virtual and physical paths | IOGraphics may establish framebuffer grouping during `start()`; upstream set the metadata afterward and used index 4 |
| `kexts/NMIntelFB/intelfb.cpp` | Publish a validated `NVRMTiming` 13-word property with pixel clock, horizontal/vertical porch/sync/total, refresh, and polarity placeholders | Supply firmware display timing to the existing upstream AGDC K5 timing-answer logic; omitted when source timings fail validation |
| `kexts/NMIntelFB/intelfb.cpp` | Return `r.phys = fApertureBase + fSurf`, `r.actualSize = pitch × height`, and `r.size` from the scanout platform-function request | Experimental exposure of the real Intel console scanout as a framebuffer resource, rather than upstream's **`r.phys = 0`** CPU-only convention. **Needs separate safety validation** |
| `kexts/NVRMAGDC/nvrm-agdc.cpp` | Change XeGfx's `AGDC_FB_CLASS` from `IONDRVFramebuffer` to `NMIntelFB` | Match the framebuffer actually installed on the Intel PCI device |
| `kexts/NVRMAGDC/nvrm-agdc.cpp` | Enumerate candidate framebuffer services, ascend to their PCI parent, and select an Intel PCI-backed `NMIntelFB` | Avoid accidentally targeting an unrelated platform framebuffer or stopping on the first unsuitable match |
| `kexts/NVRMAGDC/nvrm-agdc.cpp` | Add limited `0x711` request diagnostics and extended-capability replies for slot 0 | Investigate colour/gamma/linearization negotiation in WindowServer; still experimental |
| `plugin/NVMTLDevice_contract.m` | Guard one `newBufferWithBytesNoCopy` import branch for `WindowServer` (`XEGFX_WS_HOST_COPY`) | Force copy fallback during investigation of suspected host-pointer/VM interactions; **diagnostic, not proven required** |
| `build/nvgfx/build_nvaccel.sh` | Change `xegfxaccel` major from `26` to **`15`** | Build the correct Sequoia accelerator binary and output layout |
| `gpu/userland/scripts/env.sh` | Derive `SDKROOT` from `xcrun` rather than pinning `MacOSX15.5.sdk` | Support installations where that exact SDK path is missing |

### New project scripts

| Script | Purpose |
|---|---|
| `build_xegfx_stack.sh` | Preflight, optional repeatable source fixes, multi-component build, artifact packaging, logs, Git patch snapshots |
| `install.sh` | Backed-up system installation of XeGfx, accelerator, framebuffer, AGDC, userspace libraries and Metal plugin |
| `boot-gpu.sh` | Explicit, guarded per-boot stages 1–7; AGDC/display setup; xgprobe/Metal validation; no automatic WindowServer restart |
| `verify-plugins.sh` | Inspect installed binaries, library links, bundle signatures and ICD JSON |
| `build.sh` | Local convenience wrapper with stale Downloads path and `--ws-copy` enabled; **not the recommended entry point** |

### Upstream fixes already inherited (do not attribute these to this branch)

The `xe` branch already implements the underlying XeGfx shim, i915/GuC bring-up, and experimental Intel display accelerator. Its history includes fixes for userptr hold-page handling, mapping/lock ordering, GPU execution, and framebuffer GGTT reservation. Examples from the upstream history include:

- XeGfx 0.0.32–0.0.35 userptr pin/hold and MMU-notifier work.
- XeGfx 0.0.40–0.0.42 changes to the mmap-offset/RCU locking path.
- Boot framebuffer GGTT reservation during stage 4 so GPU allocations avoid the firmware scanout.
- NVAccel's normal IOSurface **copy presentation path** and buffer lifetime handling.

Do not revert those upstream mechanisms merely because a similarly named VM panic was also traced to firmware. Conversely, an upstream feature's presence does not prove it is fully safe on `8086:7D55`.

## Firmware memory panic and LegionCbmrFix

During testing, macOS panicked with:

```text
vm_page_validate_no_references: page 0x18001f000 is referenced @vm_resident.c:731
vm_page_validate_no_references: page 0x180028000 is referenced @vm_resident.c:731
```

The same physical pages were documented in [`watchthelight-labs/LegionCbmrFix`](https://github.com/watchthelight-labs/LegionCbmrFix). On the affected Lenovo firmware, Microsoft's `cbmr_driver.efi` occupies **41 pages (164 KiB)** starting at **`0x180000000`**. The upstream workaround unloads that image and reserves its former allocation using UEFI `EfiReservedMemoryType` before macOS can reuse it.

On this development machine, the supplied **`tools/cbmr-status.sh` from the LegionCbmrFix repository** has confirmed:

```text
cbmr     found and unloaded
range    0x180000000, 41 pages (164 KiB)
reserve  success
```

This is unusually strong evidence that a firmware memory-ownership issue contributed to the observed panic; it is **not yet proof of long-term stability**. Test before and after any graphics change, and keep a note of the exact panic page if a further crash occurs.

**LegionCbmrFix is a separate project, not built or installed by this repository.** Its UEFI driver must be configured in OpenCore independently. It is firmware-specific: do **not** assume it is safe or appropriate on arbitrary machines without confirming the actual loaded image and UEFI memory map. The UEFI unload/reserve operation and its risks are described in that project's documentation.

The firmware discovery changes how earlier experiments should be interpreted:

- Disabling NVMTL direct host-pointer imports did **not** prevent the observed panic before the firmware workaround.
- Stage-7 bring-up without restarting WindowServer appeared stable; accelerating WindowServer increased VM/GPU activity and exposed the fault more reliably. That does not prove WindowServer or ANV created the problematic mapping.
- Temporary VM logging (`phys-cache`) did not establish that XeGfx's fallback physical-map cache was responsible.
- Keep **normal fallback copying** available; optional host-import restrictions should be tested independently after establishing firmware stability.

## Known issues, experiments, and limitations

### 1. Experimental Intel physical scanout export

Upstream's `NMIntelFB` intentionally tells the accelerator that the Intel panel has a CPU mapping **but no NVIDIA-style physical VRAM resource**:

```cpp
r->kva = (void *)fSurfMap->getVirtualAddress();
r->phys = 0;
r->actualSize = 0;
```

This branch instead exports the real `fApertureBase + fSurf` address and `pitch × height`. That may cause `NVDisplayPipe::initFramebufferResource()` to create an `NVVidMemory` resource and `NVResource::getApertureMemoryDescriptor()` to construct an `IODeviceMemory::withRange()` mapping. **This is a substantive memory-model change, not only a performance switch.** It is not established that the underlying address represents a safe contiguous host-physical allocation for arbitrary consumers. Reproduce/test it separately from LegionCbmrFix or NVMTL changes. A conservative rollback is upstream's CPU-only `phys = 0`/`actualSize = 0` behaviour.

### 2. AGDC `0x711` capability implementation is partial

The branch adds colour/gamma/linearization handling and diagnostic logs, but does not establish complete hardware support. In the added direct-reply branch the outer test accepts `type == 1 || type == 2 || type == 4`; a later `else if (type == 8)` in that same branch is therefore **unreachable**. Type 8 may still be handled by the inherited AGDC answer code afterward, but the local branch itself does not implement its advertised type-8 path. Scaler capability replies are not equivalent to a working hardware scaler.

### 3. WindowServer host-import guard remains committed

The local `XEGFX_WS_HOST_COPY` change is **still in `plugin/NVMTLDevice_contract.m`** in the pushed branch. Copy-only settings previously added to `plugin/nvmtl_vk.c` during diagnostics were not part of this branch comparison. When testing direct imports again, audit both source-level guards and any deployed plugin version. The normal copy fallback must remain for unsupported/non-importable buffers.

### 4. Experimental driver behaviour

- Activation is manual and is intentionally not performed during ordinary macOS boot.
- WindowServer may fail, hang, or cause a kernel panic despite successful stage checks.
- Some DRM ioctls may return errors for unsupported/invalid requests. Debug failed command submissions instead of disabling all error reporting.
- Metal feature coverage, power management, sleep/wake, alternative display configurations, and long-term system stability are not validated here.
- The GPU's shared Vulkan memory heap and the small macOS “VRAM Total” reporting field are **not** interchangeable measures of dedicated VRAM.
- This setup is tested only on the specified Intel GPU/Sequoia environment. Other Meteor Lake IDs or newer macOS versions require additional validation.

## Rollback and recovery

### Return to a safe boot

Keep an OpenCore configuration/USB capable of booting without activating the XeGfx stages. `boot-gpu.sh` does **not** add a startup job or restart WindowServer automatically; ordinarily, a reboot clears manual acceleration state. Never remove your known-good panel/EFI recovery route while developing this driver.

### Restore a previous NVMTLDriver bundle

`install.sh` saves the previous **complete bundle** under `/Library/XeGfx/full-install-.../NVMTLDriver.bundle`. Do not overwrite a running binary. Prefer restoring a verified backup while the component is inactive or from recovery, re-sign if required, and reboot. The install script does **not** provide a dedicated automatic `--rollback` operation.

### Uninstall upstream XeGfx stack

The wrapped upstream installer implements an uninstall mode:

```bash
# REVIEW the upstream install script and backups before executing.
cd ~/src/NvGfx
sudo env XEGFX_MODE=uninstall bash gpu/xegfx/tools/install.sh
```

This removes the XeGfx-related kexts and installed ANV/libdrm assets and rebuilds the Auxiliary Kernel Collection. It does **not** automatically restore the original Metal bundle saved by `install.sh`; manage that backup separately. Uninstall from a working fallback environment and keep the previous Auxiliary Kernel Collection backup.

### Undo an experimental source edit

Use the wrapper's saved `patches/originals/` and Git diff snapshots for the specific build run, or carefully revert the relevant Git hunk. Avoid a blanket reset if unrelated branch changes are present. For the original NVMTL implementation, distinguish the `NVMTLDevice_contract.m` committed `XEGFX_WS_HOST_COPY` guard from any separately edited `nvmtl_vk.c`.

## Maintaining the fork

Upstream's `xe` branch is independent of this machine's experimental changes. Before rebasing, inspect the delta:

```bash
cd ~/src/NvGfx
git fetch upstream xe
git status --short
git log --oneline upstream/xe..meteor-lake-local
git diff --stat upstream/xe...meteor-lake-local
git diff upstream/xe...meteor-lake-local -- \
  kexts/NMIntelFB/intelfb.cpp \
  kexts/NVRMAGDC/nvrm-agdc.cpp \
  plugin/NVMTLDevice_contract.m \
  build/nvgfx/build_nvaccel.sh
```

The build wrapper automatically archives an upstream-base patch series if the configured local upstream ref exists. Preserve `MANIFEST.txt`, successful logs, test outputs, the installed kext hashes, the Sequoia build number, firmware version, and the LegionCbmrFix status when reporting issues. Changes to the scanout ABI, AGDC replies, and NVMTL host-import policy should each be tested in isolation.

**Suggested future cleanups (not yet done):** make root `build.sh` call `./build_xegfx_stack.sh` without `--ws-copy`; remove generated `.DS_Store`/compiled outputs from Git; move diagnostic host-copy behaviour behind an explicit build-time switch; correct/document the AGDC type-8 branch; and add a dedicated provenance/installation manifest for the exact userspace libraries.

## Credits and licensing

- [NullMoth Systems](https://github.com/nullmoth/nvidia-macos-driver): original NvGfx/NVMTL foundation and associated graphics implementation.
- [watchthelight-labs/NvGfx (`xe`)](https://github.com/watchthelight-labs/NvGfx/tree/xe): upstream experimental Intel XeGfx port, FreeBSD/LinuxKPI/XNU integration, scripts, and tests.
- [watchthelight-labs/LegionCbmrFix](https://github.com/watchthelight-labs/LegionCbmrFix): independent firmware-memory workaround for the matching XNU panic signature.
- [Mesa](https://mesa3d.org/): ANV Intel Vulkan implementation; [Intel Linux graphics/i915](https://docs.kernel.org/gpu/i915.html): GPU kernel-driver foundation.
- `jwx29/NvGfx`: local Meteor Lake-specific modifications and integration scripts described above.

**Required notice:** Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com).

The upstream NvGfx driver is **source-available, not generally open source**, and is licensed under the **PolyForm Noncommercial License 1.0.0**; consult `LICENSE` and `NOTICE` for the driver and individual third-party components. This README does not change or grant additional licence rights.
