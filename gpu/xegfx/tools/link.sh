#!/bin/bash
# Links out/obj (from harness/score.py --obj) + the XNU side + generated stubs into out/XeGfx.kext and runs kcabi's
# linkcheck on it.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK=${SDKROOT:-/Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk}
K="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
O="$ROOT/out/link"; rm -rf "$O"; mkdir -p "$O"
python3 "$ROOT/tools/kpi_split.py"
[ -s "$ROOT/out/kpi-clash.txt" ] && { echo "STOP: ABI clash, see out/kpi-clash.txt"; exit 1; }
python3 "$ROOT/tools/stubs.py"
xcrun clang -arch x86_64 -mmacosx-version-min=15.0 -mkernel -ffreestanding -nostdinc -O2 -c \
  "$ROOT/shim/src/stubs/xg_stubs.c" -o "$O/xg_stubs.o"
for cpp in "$ROOT"/shim/src/xnu/*.cpp; do
  xcrun clang++ -arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -mkernel -nostdinc -isysroot "$SDK" -I"$K" \
    -I"$ROOT/shim/include" -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -std=c++17 -fno-rtti -fno-exceptions \
    -O2 -Wall -Wno-inconsistent-missing-override -Wno-deprecated-declarations -c "$cpp" -o "$O/$(basename "$cpp").o"
done
B="$ROOT/out/XeGfx.kext/Contents"; rm -rf "$ROOT/out/XeGfx.kext"; mkdir -p "$B/MacOS"
find "$ROOT/out/obj" -name '*.o' > "$O/objs.txt"
xcrun clang++ -arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext \
  -Xlinker -filelist -Xlinker "$O/objs.txt" "$O/xg_stubs.o" "$O"/*.cpp.o -o "$B/MacOS/XeGfx" 2> "$O/ld.txt" || {
  grep -E 'Undefined|duplicate|error' "$O/ld.txt" | head -40; exit 1; }
cp "$ROOT/kext/Info.plist" "$B/Info.plist"
# GuC only for now: HuC and GSC need MEI (none on macOS), DMC only matters once i915 owns the display.
# Blobs come from linux-firmware (LICENSE.i915, redistributable) and are not in this repo.
FW=${XEGFX_FW:-$HOME/src/xe-src/linux-firmware}
if [ -f "$FW/mtl_guc_70.bin" ]; then
  mkdir -p "$B/Resources"
  cp "$FW/mtl_guc_70.bin" "$FW/LICENSE.i915" "$B/Resources/"
fi
codesign -s - -f "$ROOT/out/XeGfx.kext" >/dev/null 2>&1 || true
echo "XeGfx.kext $(stat -f %z "$B/MacOS/XeGfx") B"
python3 ~/src/kcabi/linkcheck.py "$ROOT/out/XeGfx.kext" | tail -25
# every undefined symbol must resolve from a declared, public KPI library (kmutil enforces this when it builds a
# collection; linkcheck only knows what the kernel exports)
python3 "$ROOT/tools/kpi_check.py" "$ROOT/out/XeGfx.kext" || exit 1
# a kernel name defined inside the kext would capture the XNU-side callers (I-094)
python3 "$ROOT/tools/shadow_check.py" "$ROOT/out/XeGfx.kext" || exit 1
