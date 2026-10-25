#!/bin/bash
# Single-run build for macOS 15 XeGfx / NvGfx 'xe' with Meteor Lake 8086:7D55.
# Does not install packages, update git remotes, install kernel extensions,
# change NVRAM, change boot-args, or restart WindowServer.
# Requires an existing configured toolchain and third-party source checkouts.
set -euo pipefail

REPO="${NVGFX_REPO:-$HOME/src/NvGfx}"
OUTBASE="${NVGFX_OUT:-$HOME/src/NvGfx-build}"
SOURCE="${XEGFX_SOURCE:-$HOME/src/xe-src}"
OGKM="${OGKM:-$HOME/src/deps/open-gpu-610.57.04}"
UPSTREAM_REF="${XEGFX_UPSTREAM_REF:-upstream/xe}"
USERLAND_CMD="${XEGFX_USERLAND_CMD:-}"
APPLY=1; BUILD_PLUGIN=1; BUILD_XLATE=0; BUILD_USERLAND=0; WS_COPY=0; CHECK_ONLY=0
usage() {
  cat <<'EOF'
Usage: bash build_xegfx_stack.sh [options]

Options:
  --repo PATH        NvGfx checkout (default ~/src/NvGfx)
  --out PATH         output base (default ~/src/NvGfx-build)
  --upstream-ref REF Git ref for Git-format patches (default upstream/xe)
  --check            dependency checks only, no source edits or builds
  --no-patches       don't apply three known, idempotent macOS 15 fixes
  --no-plugin        skip rebuilding NVMTLDriver binary
  --with-xlate       also rebuild libnvmtl_translate.dylib (needs reference pkg)
  --with-userland    invoke your existing libdrm/Mesa build command from
                     XEGFX_USERLAND_CMD (not provided by this script)
  --ws-copy          OPTIONAL experimental WindowServer host-buffer copy patch
  --help

Environment: XEGFX_SOURCE, OGKM, NVGFX_DEPS, SDKROOT, OUTROOT, XEGFX_FW,
             XEGFX_DRM_INC, NVGFX_REF_PKGROOT, XEGFX_USERLAND_CMD.
Everything is archived outside the git checkout; no automatic commits.
EOF
}
while [ "$#" -gt 0 ]; do
  case "$1" in
    --repo|--out|--upstream-ref)
      option="$1"; [ "$#" -ge 2 ] || { echo "Missing value for $option" >&2; exit 2; }
      case "$option" in
        --repo) REPO="$2";; --out) OUTBASE="$2";; --upstream-ref) UPSTREAM_REF="$2";;
      esac; shift 2 ;;
    --check) CHECK_ONLY=1; shift;;
    --no-patches) APPLY=0; shift;;
    --no-plugin) BUILD_PLUGIN=0; shift;;
    --with-xlate) BUILD_XLATE=1; shift;;
    --with-userland) BUILD_USERLAND=1; shift;;
    --ws-copy) WS_COPY=1; shift;;
    --help|-h) usage; exit 0;;
    *) echo "Unknown argument: $1" >&2; usage; exit 2;;
  esac
done
[ "$WS_COPY" -eq 0 ] || [ "$APPLY" -eq 1 ] || { echo '--ws-copy requires patches enabled' >&2; exit 2; }
# Canonicalize paths before running the build scripts.
mkdir -p "$OUTBASE"
OUTBASE="$(cd "$OUTBASE" && pwd -P)"
if [ ! -d "$REPO/.git" ] && ! git -C "$REPO" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "ERROR: NvGfx git repository not found at $REPO" >&2; exit 2
fi
REPO="$(cd "$REPO" && pwd -P)"
export OGKM
export OUTROOT="${OUTROOT:-$OUTBASE/kexts}"
export NVGFX_OUT="$OUTBASE"
export XEGFX_FW="${XEGFX_FW:-$SOURCE/linux-firmware}"
export XEGFX_DRM_INC="${XEGFX_DRM_INC:-$HOME/src/xe-userland/drm/include/drm}"
if [ -z "${SDKROOT:-}" ]; then
  if [ -d /Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk ]; then
    export SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk
  else
    export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
    echo "NOTE: macOS 15.5 SDK not found; using $SDKROOT" >&2
  fi
fi

bad=0
require_file() { [ -f "$1" ] || { echo "MISSING FILE: $1" >&2; bad=1; }; }
require_dir() { [ -d "$1" ] || { echo "MISSING DIRECTORY: $1" >&2; bad=1; }; }
require_cmd() { command -v "$1" >/dev/null 2>&1 || { echo "MISSING COMMAND: $1" >&2; bad=1; }; }
if [ "$(uname -s)" != Darwin ]; then
  echo 'ERROR: Build must run on macOS x86_64 (not Linux, Windows or GitHub Linux runner).' >&2
  [ "$CHECK_ONLY" -eq 1 ] || exit 2
fi
require_cmd git; require_cmd xcrun; require_cmd python3; require_cmd codesign
require_cmd clang; require_cmd shasum; require_cmd tee; require_cmd ditto; require_cmd tar
require_file "$REPO/build/nvgfx/build_xegfx.sh"
require_file "$REPO/build/nvgfx/build_nmintelfb.sh"
require_file "$REPO/build/nvgfx/build_xegfxagdc.sh"
require_file "$REPO/build/nvgfx/build_nvaccel.sh"
require_file "$REPO/build/nvgfx/build_plugin.sh"
require_file "$REPO/gpu/xegfx/harness/score.py"
require_file "$REPO/gpu/xegfx/tools/link.sh"
require_file "$REPO/gpu/xegfx/shim/src/xnu/xg_kext.cpp"
require_file "$REPO/kexts/NMIntelFB/intelfb.cpp"
require_file "$REPO/kexts/NVRMAGDC/nvrm-agdc.cpp"
require_file "$REPO/kexts/NVRM/accel/nvrm-accel.cpp"
require_file "$REPO/gpu/xkpi/exclude.txt"
require_file "$SOURCE/drm-kmod/drm/Makefile"
require_file "$SOURCE/freebsd/sys/compat/linuxkpi/common/include/linux/compiler_types.h"
require_dir "$SOURCE/linux"
require_file "$XEGFX_FW/mtl_guc_70.bin"
require_file "$XEGFX_FW/LICENSE.i915"
require_file "$HOME/src/kcabi/linkcheck.py"
require_file "$OGKM/version.mk"
require_dir "$SDKROOT/System/Library/Frameworks/Kernel.framework/Headers"
if [ "$BUILD_PLUGIN" -eq 1 ]; then require_file "$REPO/plugin/NVMTLDevice.m"; require_dir "${NVGFX_DEPS:-$HOME/src/deps}/Vulkan-Headers/include/vulkan"; fi
if [ "$BUILD_XLATE" -eq 1 ]; then
  require_cmd cargo
  require_file "$REPO/build/nvgfx/build_xlate.sh"
  if [ -z "${NVGFX_REF_PKGROOT:-}" ]; then
    echo "NOTICE: --with-xlate expects a reference plugin package root (NVGFX_REF_PKGROOT); upstream xlate script compares exports against it." >&2
  fi
fi
if [ "$BUILD_USERLAND" -eq 1 ] && [ -z "$USERLAND_CMD" ]; then
  echo 'ERROR: --with-userland requires XEGFX_USERLAND_CMD (your existing Mesa/libdrm build script).' >&2; bad=1
fi
if [ "$CHECK_ONLY" -eq 1 ]; then
  [ "$bad" -eq 0 ] && echo 'Preflight passed.' || echo 'Preflight failed.'
  exit "$bad"
fi
[ "$bad" -eq 0 ] || { echo 'Preflight failed. Build aborted without editing source.' >&2; exit 2; }

RUN="$OUTBASE/runs/$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$RUN"/{logs,patches,artifacts} "$REPO/gpu/xegfx/out"
LOG="$RUN/logs/build-all.log"
log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "$LOG"; }
trap 'rc=$?; printf "\nBuild exit status: %s\nRun directory: %s\n" "$rc" "$RUN" | tee -a "$LOG"; exit "$rc"' EXIT

# Export Git worktree and history separately. This does not stage, reset, fetch or commit.
snapshot_git() {
  phase="$1"; dir="$RUN/patches/$phase"; mkdir -p "$dir"
  git -C "$REPO" status --porcelain=v1 -uall > "$dir/status.txt"
  git -C "$REPO" diff HEAD --binary > "$dir/worktree-vs-HEAD.patch"
  git -C "$REPO" diff --binary > "$dir/unstaged.patch"
  git -C "$REPO" diff --cached --binary > "$dir/staged.patch"
  git -C "$REPO" ls-files --others --exclude-standard > "$dir/untracked.txt"
  git -C "$REPO" rev-parse HEAD > "$dir/HEAD.txt"
  git -C "$REPO" log -1 --format='%H %ai %s' > "$dir/commit.txt"
  if git -C "$REPO" rev-parse --verify "$UPSTREAM_REF^{commit}" >/dev/null 2>&1; then
    base="$(git -C "$REPO" merge-base HEAD "$UPSTREAM_REF")"
    printf '%s\n' "$base" > "$dir/upstream-merge-base.txt"
    git -C "$REPO" diff --binary "$base" HEAD > "$dir/committed-vs-upstream.patch"
    git -C "$REPO" diff --binary "$base" > "$dir/full-tracked-changes-vs-upstream.patch"
    mkdir -p "$dir/commit-series"
    git -C "$REPO" format-patch --quiet -o "$dir/commit-series" "$base..HEAD" || true
  else
    printf 'Ref %s missing. No fetch performed; committed patch series not generated.\n' "$UPSTREAM_REF" > "$dir/upstream-note.txt"
  fi
  # Archive only untracked source/patch/config files, not giant generated build trees.
  grep -E '\.(c|cc|cpp|h|hpp|m|mm|py|sh|plist|patch|diff|md|yml|yaml|toml|rs)$' "$dir/untracked.txt" \
    > "$dir/untracked-source-files.txt" || true
  if [ -s "$dir/untracked-source-files.txt" ]; then
    (cd "$REPO" && tar -czf "$dir/untracked-source-files.tar.gz" -T "$dir/untracked-source-files.txt")
  fi
}
log "Repo: $REPO / $(git -C "$REPO" rev-parse --short HEAD)"
log "Output: $RUN"
log "SDK: $SDKROOT"
log 'Saving Git status/diffs BEFORE modifications'
snapshot_git before

if [ "$APPLY" -eq 1 ]; then
  log 'Applying 3 known, idempotent source fixes; optional WindowServer copy fix if requested'
  # Explicit exact-match transformations; fail closed on unfamiliar upstream source.
  python3 - "$REPO" "$RUN/patches" "$WS_COPY" <<'PY'
import pathlib, re, sys, shutil
root, archive, ws = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), int(sys.argv[3])
originals = archive/'originals'
originals.mkdir(parents=True, exist_ok=True)
report=[]
def modify(rel, callback):
    p=root/rel
    original=p.read_text()
    new=callback(original)
    if new != original:
        b=originals/rel; b.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(p,b)
        p.write_text(new)
        report.append((rel,'MODIFIED'))
    else: report.append((rel,'already applied / unchanged'))

def fix_fb(s):
    needle='setProperty("IOFBDependentIndex", 4ull, 32);'
    target='setProperty("IOFBDependentIndex", 0ull, 32);'
    new=s.replace(needle,target)
    if new.count(target) < 2: raise RuntimeError('NMIntelFB expected two IOFBDependentIndex declarations at 0/4; source differs')
    return new

def fix_agdc(s):
    pre,sep,tail=s.partition('#ifdef NMX_XEGFX')
    if not sep: raise RuntimeError('NMX_XEGFX AGDC block missing')
    head, sep2, rest = tail.partition('#else')
    if not sep2: raise RuntimeError('AGDC XeGfx #else missing')
    old='#define AGDC_FB_CLASS     "IONDRVFramebuffer"'
    new='#define AGDC_FB_CLASS     "NMIntelFB"'
    if old not in head and new not in head: raise RuntimeError('AGDC target unknown; refusing unsafe patch')
    return pre+sep+head.replace(old,new)+sep2+rest

def fix_accel(s):
    old='names=(XeGfxAccel); majors=(26)'
    new='names=(XeGfxAccel); majors=(15)'
    if old not in s and new not in s: raise RuntimeError('XeGfxAccel macOS selection changed; check upstream')
    return s.replace(old,new)

def fix_copy(s):
    old='if (nvmtl_vk_buffer_import_host(a0, a1, &b->_b) == 0) {'
    new=('/* XEGFX_WS_HOST_COPY: prevent WindowServer importing read-only pages as writable GPU host memory. */\n'
         '    const char *xegfx_proc = getprogname();\n'
         '    const bool xegfx_ws_copy = xegfx_proc && strcmp(xegfx_proc, "WindowServer") == 0;\n'
         '    if (!xegfx_ws_copy && nvmtl_vk_buffer_import_host(a0, a1, &b->_b) == 0) {')
    if 'XEGFX_WS_HOST_COPY' in s: return s
    if s.count(old)!=1: raise RuntimeError('Cannot safely locate host-import branch; upstream changed')
    return s.replace(old,new)

modify('kexts/NMIntelFB/intelfb.cpp',fix_fb)
modify('kexts/NVRMAGDC/nvrm-agdc.cpp',fix_agdc)
modify('build/nvgfx/build_nvaccel.sh',fix_accel)
if ws: modify('plugin/NVMTLDevice_contract.m',fix_copy)
(archive/'applied-fixes.txt').write_text(''.join('%s\t%s\n'%(a,b) for a,b in report))
print('\n'.join('%s: %s'%(a,b) for a,b in report))
PY
else
  log 'Source fixes disabled (--no-patches): YOU must ensure macOS 15 XeGfxAccel and NMIntelFB/AGDC are correct.'
fi
snapshot_git after-patching
cat > "$RUN/PATCHES.md" <<'EOF'
# Patch inventory — XeGfx / Meteor Lake / macOS 15

All tracked changes are saved in `patches/before/`, `patches/after-patching/`,
then `patches/finished/`. This wrapper neither commits nor overwrites your
existing commits. `git diff` includes pre-existing manual edits. `git format-patch`
exports committed changes relative to `XEGFX_UPSTREAM_REF`, if present locally.

## Idempotent compatibility edits applied by default

- `kexts/NMIntelFB/intelfb.cpp`: both `IOFBDependentIndex` setters become **0**
  instead of 4. (IOGraphics dependent index, *not* XeGfxAccel's head 4.)
- `kexts/NVRMAGDC/nvrm-agdc.cpp`: under `NMX_XEGFX`, select the actual
  `NMIntelFB` provider instead of an `IONDRVFramebuffer`.
- `build/nvgfx/build_nvaccel.sh`: XeGfxAccel targets macOS **15**, not the
  default macOS 26 build. Output `kexts/XeGfxAccel/15/XeGfxAccel.kext`.

Optional `--ws-copy` also changes `plugin/NVMTLDevice_contract.m` to force
WindowServer host buffers through the copy fallback instead of importing host
pages directly. It is an *experiment* for `VK_ERROR_DEVICE_LOST`, not a proven
patch for this laptop. A copy of each source file before **this script's own**
changes is saved in `patches/originals/`.

## Applying/exporting upstream merges

- `patches/finished/committed-vs-upstream.patch`: committed delta only
- `patches/finished/full-tracked-changes-vs-upstream.patch`: commits plus
  uncommitted tracked edits
- `patches/finished/worktree-vs-HEAD.patch`: uncommitted tracked edits only
- `patches/finished/commit-series/`: individual Git-formatted commits
- `patches/finished/untracked-source-files.tar.gz`: untracked code, if present

Git cannot include *untracked* files in `git diff`; they are listed separately
and archived above. To make your work ready for normal Git merges, review and
commit the wanted changes in your repo after the build succeeds.
EOF

step() {
  name="$1"; shift
  log "START: $name"
  if ( "$@" ) 2>&1 | tee "$RUN/logs/${name}.log"; then
    log "PASS: $name"
  else
    log "FAIL: $name. See $RUN/logs/${name}.log"
    return 1
  fi
}

# Build order: framebuffer, AGDC, accelerator, XeGfx itself, then Metal plugin.
step NMIntelFB bash "$REPO/build/nvgfx/build_nmintelfb.sh"
step XeGfxAGDC bash "$REPO/build/nvgfx/build_xegfxagdc.sh"
step XeGfxAccel bash "$REPO/build/nvgfx/build_nvaccel.sh" xegfxaccel
step XeGfx bash "$REPO/build/nvgfx/build_xegfx.sh"
if [ "$BUILD_PLUGIN" -eq 1 ]; then step NVMTLDriver bash "$REPO/build/nvgfx/build_plugin.sh"; fi
if [ "$BUILD_XLATE" -eq 1 ]; then step libnvmtl_translate bash "$REPO/build/nvgfx/build_xlate.sh"; fi
if [ "$BUILD_USERLAND" -eq 1 ]; then
  # Explicitly supplied shell command, never guessed from an unknown checkout.
  step Mesa-libdrm bash -lc "$USERLAND_CMD"
fi

# Bundle successful outputs WITHOUT touching /Library/Extensions or the running system.
copy_kext() {
  local name="$1" source="$2"
  [ -d "$source/Contents" ] || { log "ERROR: missing build: $source"; return 1; }
  ditto "$source" "$RUN/artifacts/$name.kext"
  codesign --force --sign - "$RUN/artifacts/$name.kext" >/dev/null 2>&1 || log "WARNING: ad-hoc signing $name failed; validate before installing"
  /usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$RUN/artifacts/$name.kext/Contents/Info.plist" 2>/dev/null || true
  shasum -a 256 "$RUN/artifacts/$name.kext/Contents/MacOS/$name" | tee -a "$RUN/artifacts/SHA256SUMS.txt"
}
copy_kext NMIntelFB "$OUTROOT/NMIntelFB/NMIntelFB.kext"
copy_kext XeGfxAGDC "$OUTROOT/XeGfxAGDC/XeGfxAGDC.kext"
copy_kext XeGfxAccel "$OUTROOT/XeGfxAccel/15/XeGfxAccel.kext"
copy_kext XeGfx "$REPO/gpu/xegfx/out/XeGfx.kext"
if [ "$BUILD_PLUGIN" -eq 1 ]; then
  cp "$NVGFX_OUT/plugin/NVMTLDriver" "$RUN/artifacts/NVMTLDriver";
  shasum -a 256 "$RUN/artifacts/NVMTLDriver" >> "$RUN/artifacts/SHA256SUMS.txt"
fi
if [ "$BUILD_XLATE" -eq 1 ]; then
  cp "$NVGFX_OUT/xlate/libnvmtl_translate.dylib" "$RUN/artifacts/"
  shasum -a 256 "$RUN/artifacts/libnvmtl_translate.dylib" >> "$RUN/artifacts/SHA256SUMS.txt"
fi
snapshot_git finished
{
  echo 'XeGfx/NvGfx build manifest'
  echo "when: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "repo: $REPO"
  echo "repo HEAD: $(git -C "$REPO" rev-parse HEAD)"
  echo "repo branch: $(git -C "$REPO" branch --show-current)"
  echo "upstream ref: $UPSTREAM_REF"
  echo "sdk: $SDKROOT"
  echo "OGKM: $OGKM"
  echo "xe-src: $SOURCE"
  echo "output: $RUN"
  echo "forced WS copy patch: $WS_COPY"
  echo 'NOTE: Mesa ANV/libdrm are NOT built unless --with-userland is supplied with XEGFX_USERLAND_CMD.'
  echo 'NOTE: no kexts were installed; no live GPU services were started.'
} > "$RUN/MANIFEST.txt"
(cd "$RUN" && tar -czf "artifacts.tar.gz" artifacts)
log "SUCCESS: Kexts and plugin in $RUN/artifacts/"
log "Git patches (committed + uncommitted), logs and manifest in $RUN/"
log 'Source modifications are intentionally left uncommitted so you can inspect, commit, and rebase/merge them yourself.'
