#!/bin/bash
# XeGfx full-stack installer for macOS Sequoia 15 / NvGfx branch xe.
# Installs *existing builds*; does not rebuild source.
#   bash install_xegfx_full.sh --check   # preflight, no changes
#   bash install_xegfx_full.sh           # full installation, no auto-reboot
#
# Wraps gpu/xegfx/tools/install.sh (kexts, ANV, libdrm, xgprobe) and
# additionally replaces NVMTLDriver.bundle's executable with the current build.
# Backups: /Library/XeGfx/full-install-YYYYmmdd-HHMMSS-PID/
set -Eeuo pipefail
umask 022

CHECK=0
case "${1:-}" in
  "") ;;
  --check) CHECK=1 ;;
  -h|--help)
    echo "Usage: bash $0 [--check]"
    echo "Set NVGFX_REPO, NVGFX_OUT, XEGFX_USERLAND to override default paths."
    exit 0 ;;
  *) echo "Unknown option: $1" >&2; exit 2 ;;
esac
if (( $# > 1 )); then
  echo "Too many arguments." >&2; exit 2
fi

# Preserve the original login user's paths instead of falling back to root's HOME.
if (( EUID != 0 )); then
  exec sudo /usr/bin/env     "NVGFX_REPO=${NVGFX_REPO:-}"     "NVGFX_OUT=${NVGFX_OUT:-}"     "XEGFX_USERLAND=${XEGFX_USERLAND:-}"     /bin/bash "$0" "$@"
fi

say()  { printf '\n==> %s\n' "$*"; }
die()  { echo "ERROR: $*" >&2; exit 1; }
need() { [[ -e "$1" ]] || die "Missing $2: $1"; }

RUN_USER="${SUDO_USER:-}"
if [[ -z "$RUN_USER" || "$RUN_USER" == root ]]; then
  RUN_USER="$(stat -f '%Su' /dev/console)"
fi
[[ "$RUN_USER" != "root" && "$RUN_USER" != "loginwindow" ]] ||
  die "Cannot determine the original login user. Run this as your normal user, not a root shell."

RUN_HOME="$(dscl . -read "/Users/$RUN_USER" NFSHomeDirectory | sed -n 's/^NFSHomeDirectory: //p')"
[[ -d "$RUN_HOME" ]] || die "Cannot locate $RUN_USER's home directory."

# Avoid accidentally using the path of a different checkout.
REPO="${NVGFX_REPO:-$RUN_HOME/src/NvGfx}"
OUT="${NVGFX_OUT:-$RUN_HOME/src/NvGfx-build}"
USERLAND="${XEGFX_USERLAND:-$RUN_HOME/src/xe-userland/prefix/lib}"
BUNDLE="/Library/GPUBundles/NVMTLDriver.bundle"
GB="/Library/GPUBundles/nvmtl"

XK="$REPO/gpu/xegfx/out/XeGfx.kext"
ACC="$OUT/kexts/XeGfxAccel/15/XeGfxAccel.kext"
IFB="$OUT/kexts/NMIntelFB/NMIntelFB.kext"
AGDC="$OUT/kexts/XeGfxAGDC/XeGfxAGDC.kext"
PLUGIN="$OUT/plugin/NVMTLDriver"
INSTALLED_PLUGIN="$BUNDLE/Contents/MacOS/NVMTLDriver"
LIBDRM="$USERLAND/libdrm.2.dylib"
ANV="$USERLAND/libvulkan_intel.dylib"

say "Preflight for NvGfx xe installation"
echo "User:      $RUN_USER"
echo "Repo:      $REPO"
echo "Builds:    $OUT"
echo "Userland:  $USERLAND"
echo "Target OS: $(sw_vers -productVersion)"
[[ "$(uname -s)" == Darwin && "$(uname -m)" == x86_64 ]] ||
  die "This installer targets x86_64 macOS."
[[ "$(sw_vers -productVersion)" == 15.* ]] ||
  die "This installer expects macOS Sequoia 15.x (XeGfxAccel/15)."
need "$REPO/gpu/xegfx/tools/install.sh" "upstream installer"
need "$BUNDLE/Contents/Info.plist" "installed NVMTLDriver.bundle"
need "$INSTALLED_PLUGIN" "currently installed NVMTLDriver"
need "$LIBDRM" "built libdrm"
need "$ANV" "built Mesa ANV"
[[ -d "$GB" ]] || die "Missing userland installation directory: $GB"
need "$PLUGIN" "new NVMTLDriver build"

# Validate kext sources (upstream treats most of them as optional).
check_kext() {
  local dir="$1" expected="$2" binary="$3" actual
  need "$dir/Contents/Info.plist" "$expected Info.plist"
  need "$dir/Contents/MacOS/$binary" "$expected executable"
  actual="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$dir/Contents/Info.plist")" ||
    die "Could not read bundle ID from $dir"
  [[ "$actual" == "$expected" ]] ||
    die "Wrong bundle ID for $dir: got $actual, expected $expected"
  echo "OK: $expected"
}
check_kext "$XK"   "org.watchthelight.XeGfx"      "XeGfx"
check_kext "$ACC"  "org.watchthelight.XeGfxAccel" "XeGfxAccel"
check_kext "$IFB"  "com.nullmoth.NMIntelFB"       "NMIntelFB"
check_kext "$AGDC" "org.watchthelight.XeGfxAGDC"  "XeGfxAGDC"

# Don't accidentally regress to AGDC targeting only IONDRVFramebuffer.
if ! strings "$AGDC/Contents/MacOS/XeGfxAGDC" | grep -Fq 'NMIntelFB'; then
  die "AGDC build does not reference NMIntelFB; rebuild your patched AGDC before installation."
fi

echo
echo "Metal plugin hashes (built, then installed):"
shasum -a 256 "$PLUGIN" "$INSTALLED_PLUGIN"
echo
echo "Userland sources:"
ls -lh "$LIBDRM" "$ANV"
if (( CHECK )); then
  say "Preflight passed. No files modified (--check)."
  exit 0
fi

STAMP="$(date +%Y%m%d-%H%M%S)-$$"
BACKUP="/Library/XeGfx/full-install-$STAMP"
STAGE="/Library/GPUBundles/.NVMTLDriver-$STAMP.bundle"
mkdir -p "$BACKUP"
PREVIOUS="$BACKUP/NVMTLDriver.bundle"
SWAP_STARTED=0

cleanup() {
  local rc=$?
  trap - EXIT
  # If the old bundle has been moved away and new one is missing,
  # restore it automatically. Never remove the saved working copy.
  if (( SWAP_STARTED )) && [[ ! -d "$BUNDLE" && -d "$PREVIOUS" ]]; then
    echo "Restoring original NVMTLDriver bundle after incomplete swap..."
    mv "$PREVIOUS" "$BUNDLE" || true
  fi
  [[ ! -d "$STAGE" ]] || rm -rf "$STAGE"
  if (( rc != 0 )); then
    echo "Installation aborted (exit $rc). Backup/logs: $BACKUP" >&2
    echo "The upstream installer may have updated some files before its failure." >&2
  fi
  exit "$rc"
}
trap cleanup EXIT

say "1/4 Stage and sign replacement NVMTLDriver bundle"
# Stage a COPY; never write into an existing loaded executable.
ditto "$BUNDLE" "$STAGE"
install -o root -g wheel -m 755 "$PLUGIN" "$STAGE/Contents/MacOS/NVMTLDriver"
cmp -s "$PLUGIN" "$STAGE/Contents/MacOS/NVMTLDriver" ||
  die "Copied plugin does not match the new build."
# The pre-existing bundle contains nested libraries (AIR runtime, translator,
# codecs); signing the staged copy updates its containing seal too.
codesign --force --deep --sign - "$STAGE" \
  >"$BACKUP/plugin-codesign.log" 2>&1 ||
  { tail -40 "$BACKUP/plugin-codesign.log"; die "Cannot sign replacement Metal bundle."; }
codesign --verify --deep --strict --verbose=2 "$STAGE" \
  >"$BACKUP/plugin-verify.log" 2>&1 ||
  { cat "$BACKUP/plugin-verify.log"; die "Replacement Metal bundle failed verification."; }

# Re-signing may modify the Mach-O signature bytes, so compare the activated
# executable with the SIGNED staging artifact rather than with the source file.
STAGED_SHA="$(shasum -a 256 "$STAGE/Contents/MacOS/NVMTLDriver" | awk '{print $1}')"
SOURCE_SHA="$(shasum -a 256 "$PLUGIN" | awk '{print $1}')"
echo "Built plugin SHA256:  $SOURCE_SHA"
echo "Signed plugin SHA256: $STAGED_SHA"

say "2/4 Run upstream XeGfx installer (kexts, libdrm, ANV, xgprobe, Auxiliary KC)"
# Upstream installer handles KC backups/inspection and swaps ANV and libdrm via
# temporary files. Explicit XEGFX_USERLAND is essential; otherwise it may skip
# userland updates without reporting an error.
env \
  XEGFX_ACCEL="$ACC" \
  XEGFX_INTELFB="$IFB" \
  XEGFX_AGDC="$AGDC" \
  XEGFX_USERLAND="$USERLAND" \
  /bin/bash "$REPO/gpu/xegfx/tools/install.sh" \
  2>&1 | tee "$BACKUP/upstream-install.log"

say "3/4 Replace Metal bundle (old bundle retained in backup directory)"
mv "$BUNDLE" "$PREVIOUS"
SWAP_STARTED=1
mv "$STAGE" "$BUNDLE" || die "Failed to activate replacement Metal plugin."
SWAP_STARTED=0

say "4/4 Verify on-disk installation"
codesign --verify --deep --strict --verbose=2 "$BUNDLE" ||
  die "Installed Metal bundle failed verification; previous version at $PREVIOUS"
ACTUAL_SHA="$(shasum -a 256 "$INSTALLED_PLUGIN" | awk '{print $1}')"
[[ "$ACTUAL_SHA" == "$STAGED_SHA" ]] ||
  die "Installed Metal executable hash does not match the signed staging artifact."
need "$GB/libdrm.2.dylib" "installed libdrm"
need "$GB/libvulkan_intel.dylib" "installed Mesa ANV"
need "$GB/intel_icd.json" "Vulkan ICD config"

for f in XeGfx XeGfxAccel NMIntelFB XeGfxAGDC; do
  need "/Library/Extensions/$f.kext/Contents/Info.plist" "installed $f.kext"
done

echo
echo "Updated Metal executable:"
shasum -a 256 "$PLUGIN" "$INSTALLED_PLUGIN"
echo
echo "Installed userland:"
ls -lh "$GB/libdrm.2.dylib" "$GB/libvulkan_intel.dylib" "$GB/intel_icd.json"
echo
echo "Backup directory: $BACKUP"
say "Done. Reboot before bringing up XeGfx or testing WindowServer."
echo "NOTE: valid ad-hoc code signing does not guarantee WindowServer can load"
echo "      the plugin. Test and inspect WindowServer logs after reboot."
