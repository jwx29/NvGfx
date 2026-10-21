#!/bin/bash
# Links out/obj (from harness/score.py --obj) + the XNU side + generated stubs into out/XeGfx.kext and runs kcabi's
# linkcheck on it.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK=${SDKROOT:-/Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk}
K="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
O="$ROOT/out/link"; rm -rf "$O"; mkdir -p "$O"
python3 "$ROOT/tools/kpi_split.py" >/dev/null
python3 "$ROOT/tools/stubs.py"
xcrun clang -arch x86_64 -mmacosx-version-min=15.0 -mkernel -ffreestanding -nostdinc -O2 -c \
  "$ROOT/shim/src/stubs/xg_stubs.c" -o "$O/xg_stubs.o"
xcrun clang++ -arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -mkernel -nostdinc -isysroot "$SDK" -I"$K" \
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -std=c++17 -fno-rtti -fno-exceptions -O2 -Wall \
  -Wno-inconsistent-missing-override -c "$ROOT/shim/src/xnu/xg_kext.cpp" -o "$O/xg_kext.o"
B="$ROOT/out/XeGfx.kext/Contents"; rm -rf "$ROOT/out/XeGfx.kext"; mkdir -p "$B/MacOS"
find "$ROOT/out/obj" -name '*.o' > "$O/objs.txt"
xcrun clang++ -arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext \
  -Xlinker -filelist -Xlinker "$O/objs.txt" "$O/xg_stubs.o" "$O/xg_kext.o" -o "$B/MacOS/XeGfx" 2> "$O/ld.txt" || {
  grep -E 'Undefined|duplicate|error' "$O/ld.txt" | head -40; exit 1; }
cp "$ROOT/kext/Info.plist" "$B/Info.plist"
codesign -s - -f "$ROOT/out/XeGfx.kext" >/dev/null 2>&1 || true
echo "XeGfx.kext $(stat -f %z "$B/MacOS/XeGfx") B"
python3 ~/src/kcabi/linkcheck.py "$ROOT/out/XeGfx.kext" | tail -25
