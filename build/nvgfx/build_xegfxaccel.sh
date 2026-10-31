#!/bin/bash
# Optional WTLIntelGfx 0.9 XeGfxAccel build from imported Intel-only sources.
# Do NOT install beside the existing NVRM-backed XeGfxAccel; test and replace
# only when you've verified compatibility with the current macOS major.
# XEGFX_ACCEL_MAJOR=15|26 (default 15 for the Meteor Lake Sequoia fork)
# OGKM must point to the open-gpu-kernel-modules version used by this source.
set -euo pipefail
. "$(dirname "$0")/kext_flags.sh"
SRC="$REPO/kexts/XeGfxAccel"
name=XeGfxAccel
major="${XEGFX_ACCEL_MAJOR:-15}"
case "$major" in 15|26) ;; *) echo "XEGFX_ACCEL_MAJOR must be 15 or 26" >&2; exit 2 ;; esac
out="$OUTROOT/$name/$major"
extra=( -DNMX_ACCEL -DNMX_XEGFX )
if [ "$major" = 26 ]; then extra+=( -DNM_TAHOE ); fi
[ -f "$SRC/accel/nvrm-accel.cpp" ] || { echo "Missing imported WTLIntelGfx XeGfxAccel sources" >&2; exit 1; }
rm -rf "$out"; mkdir -p "$out/$name.kext/Contents/MacOS"
"$CXX" "${KFLAGS[@]}" -I"$SRC" "${RMINC[@]}" "${KMSINC[@]}" "${RMDEFS[@]}" "${extra[@]}" \
  -I"$SRC/accel" -I"$SRC/accel/iofam" -I"$SRC/accel/re" \
  -c "$SRC/accel/nvrm-accel.cpp" -o "$out/accel.o" 2>"$out/accel.err" || {
    grep -A2 'error:' "$out/accel.err" | head -40 || true
    exit 1
  }
"$CXX" "${LDFLAGS[@]}" "$out/accel.o" -o "$out/$name.kext/Contents/MacOS/$name" \
  2>"$out/accelld.err" || { head -50 "$out/accelld.err"; exit 1; }
cp "$SRC/accel/xe/Info.plist" "$out/$name.kext/Contents/Info.plist"
codesign -s - -f "$out/$name.kext" >/dev/null 2>&1 || true
echo "Upstream XeGfxAccel for macOS $major -> $out/$name.kext"
