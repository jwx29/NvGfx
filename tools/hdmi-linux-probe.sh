#!/usr/bin/env bash
# Read-only Intel HDMI/DRM connector survey: run on Linux with HDMI connected.
set -u
shopt -s nullglob
echo "XeGfx Linux HDMI connector report"
date -Is
uname -a
echo "=== PCI graphics and drivers ==="
if command -v lspci >/dev/null 2>&1; then
    lspci -nnk | grep -A5 -Ei 'VGA compatible|Display controller|3D controller' || :
else
    echo "lspci not found; install pciutils"
fi
echo "=== DRM connector status, modes and EDID ==="
found=0
for f in /sys/class/drm/card*-*/status; do
    [ -r "$f" ] || continue
    found=$((found + 1))
    connector=${f%/status}
    printf '%s: %s\n' "${connector##*/}" "$(cat "$f")"
    if [ -r "$connector/enabled" ]; then
        printf '  enabled: %s\n' "$(cat "$connector/enabled")"
    fi
    if [ -r "$connector/modes" ]; then
        echo '  first 15 modes:'
        sed -n '1,15p' "$connector/modes" | sed 's/^/    /'
    fi
    if [ -r "$connector/edid" ]; then
        printf '  EDID bytes: %s\n' "$(wc -c < "$connector/edid" | tr -d ' ')"
        if command -v edid-decode >/dev/null 2>&1; then
            edid-decode "$connector/edid" 2>/dev/null | grep -E 'Display Product Name|Manufacturer|DTD|VIC|native' | head -20 || :
        fi
    fi
    readlink -f "$connector/device" 2>/dev/null || :
done
if [ "$found" -eq 0 ]; then echo "No DRM connectors found"; fi
echo "=== DRM card drivers ==="
for f in /sys/class/drm/card[0-9]*/device/driver; do
    [ -L "$f" ] || continue
    printf '%s: %s\n' "$f" "$(readlink -f "$f")"
done
echo "=== DRM atomic KMS state (connector, encoder, CRTC, plane) ==="
debug_state=0
for f in /sys/kernel/debug/dri/*/state; do
    [ -f "$f" ] || continue
    debug_state=$((debug_state + 1))
    if [ -r "$f" ]; then
        echo "===== $f ====="
        sed -n '1,1100p' "$f"
    else
        echo "$f: permission denied; try sudo bash tools/hdmi-linux-probe.sh"
    fi
done
if [ "$debug_state" -eq 0 ]; then
    echo "No DRM atomic KMS debug state found; inspect debugfs mount and kernel config"
fi
echo "=== Optional modetest connector/encoder/CRTC enumeration ==="
if command -v modetest >/dev/null 2>&1; then
    modetest -M i915 -c -e -p 2>&1 | sed -n '1,280p' || :
else
    echo "modetest not installed (usually provided by libdrm-tests)"
fi
echo "=== Optional drm_info output ==="
if command -v drm_info >/dev/null 2>&1; then
    drm_info 2>&1 | sed -n '1,260p' || :
else
    echo "drm_info not installed; optional only"
fi
echo "=== Optional i915 connector-to-DDI diagnostic ==="
for f in /sys/kernel/debug/dri/*/i915_display_info; do
    [ -f "$f" ] || continue
    if [ -r "$f" ]; then
        echo "$f"
        sed -n '1,450p' "$f"
    else
        echo "$f is not readable (run with sudo and mounted debugfs)"
    fi
done
echo "=== Recent kernel display/HDMI messages ==="
dmesg 2>/dev/null | grep -Ei 'i915|drm|xe.*(hdmi|connector|port)|HDMI|DDI|EDID|hotplug|HPD' | tail -110 || :
echo "Completed: no hardware registers were written"
