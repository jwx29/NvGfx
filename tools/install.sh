#!/bin/bash
# Installs out/XeGfx.kext into /Library/Extensions and rebuilds the auxiliary kernel collection beside the old one,
# keeping a backup and checking that every third-party kext of the old collection is still in the new one.
# Run as root. XEGFX_MODE=uninstall removes it again. The kext loads idle; sysctl debug.xegfx_start=1 starts it.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd); EXT=/Library/Extensions; KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
K=/System/Library/Kernels/kernel; KARG=(--allow-missing-kdk); [ -f "$K" ] && KARG+=(--kernel "$K")
KB=/System/Library/KernelCollections/BootKernelExtensions.kc; KS=/System/Library/KernelCollections/SystemKernelExtensions.kc
die() { echo "xegfx: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root"
MODE=${XEGFX_MODE:-install}
BK=/Library/XeGfx/backup-$(date +%Y%m%d-%H%M%S); mkdir -p "$BK"; [ -f "$KC" ] && cp -p "$KC" "$BK/"
OLD=$(kmutil inspect -a x86_64 -A "$KC" 2>/dev/null | awk -F'\t' '$1 ~ /^[a-z]+\.[A-Za-z0-9.-]+$/ && $1 !~ /^com\.apple\./ && $1 != "org.watchthelight.XeGfx" {print $1}' || true)
if [ "$MODE" = install ]; then
  rm -rf "$EXT/XeGfx.kext.new"; ditto "$ROOT/out/XeGfx.kext" "$EXT/XeGfx.kext.new"
  chown -R root:wheel "$EXT/XeGfx.kext.new"; chmod -R 755 "$EXT/XeGfx.kext.new"
  rm -rf "$EXT/XeGfx.kext"; mv "$EXT/XeGfx.kext.new" "$EXT/XeGfx.kext"
else
  rm -rf "$EXT/XeGfx.kext"
fi
NEW="$KC.xegfx-new"; rm -f "$NEW"
kmutil create -n aux --volume-root / "${KARG[@]}" -B $KB -S $KS --repository "$EXT" -A "$NEW" -z > "$BK/kmutil.log" 2>&1 \
  || die "collection build failed (log $BK/kmutil.log; old collection kept)"
INS=$(kmutil inspect -a x86_64 -A "$NEW" 2>/dev/null) || die "cannot inspect the new collection"
if [ "$MODE" = install ]; then printf '%s\n' "$INS" | grep -q 'org.watchthelight.XeGfx' || die "XeGfx missing from the new collection"
else printf '%s\n' "$INS" | grep -q 'org.watchthelight.XeGfx' && die "XeGfx still in the new collection"; fi
for b in $OLD; do printf '%s\n' "$INS" | grep -q "^$b[[:space:]]" || die "$b was in the old collection but is missing from the new one"; done
mv -f "$NEW" "$KC"
echo "xegfx: $MODE done (backup $BK, kept: $(echo $OLD | wc -w | tr -d ' ') kexts); reboot to apply"
