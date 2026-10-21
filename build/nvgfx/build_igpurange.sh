#!/bin/bash
# build_nmigpurange.sh: IGPUConsoleRange.kext (early console range for IONDRV on the iGPU) as one translation unit.
# <wtl> fork: nvgfx | author: watchthelight </wtl>
#   SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk build/nvgfx/build_nmigpurange.sh
#   -> $OUTROOT/IGPUConsoleRange/IGPUConsoleRange.kext (OUTROOT default ~/src/NvGfx-build/kexts)
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
SDK=${SDKROOT:-/Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk}
OUT=${OUTROOT:-$HOME/src/NvGfx-build/kexts}/IGPUConsoleRange
K="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
[ -d "$K" ] || { echo "STOP: no Kernel.framework headers in $SDK"; exit 1; }
SRC="$REPO/kexts/IGPUConsoleRange"
rm -rf "$OUT/IGPUConsoleRange.kext"; mkdir -p "$OUT/IGPUConsoleRange.kext/Contents/MacOS"
FLAGS=(-arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -mkernel -nostdinc -isysroot "$SDK" -I"$K" -I"$SRC"
       -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
       -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common -O2 -Wall -Werror -Wno-unused-parameter -g0
       -Wno-deprecated-declarations -Wno-inconsistent-missing-override -Wno-overloaded-virtual -Wno-unused-function -Wno-sign-conversion)
xcrun clang++ "${FLAGS[@]}" -c "$SRC/igpurange.cpp" -o "$OUT/igpurange.o"
xcrun clang++ -arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext \
  "$OUT/igpurange.o" -o "$OUT/IGPUConsoleRange.kext/Contents/MacOS/IGPUConsoleRange"
cp "$SRC/Info.plist" "$OUT/IGPUConsoleRange.kext/Contents/Info.plist"
codesign -s - -f "$OUT/IGPUConsoleRange.kext" >/dev/null 2>&1 || true
echo "IGPUConsoleRange $(shasum -a 256 "$OUT/IGPUConsoleRange.kext/Contents/MacOS/IGPUConsoleRange" | cut -c1-12) $(stat -f %z "$OUT/IGPUConsoleRange.kext/Contents/MacOS/IGPUConsoleRange") B -> $OUT/IGPUConsoleRange.kext"
