#!/bin/bash
# XeGfx manual per-boot bring-up for macOS Sequoia / Intel Meteor Lake.
# Uses installed Auxiliary Kernel Collection kexts; does not install or rebuild.
# Does NOT restart WindowServer, enable startup automation, or change OpenCore.
# On a healthy NMIntelFB boot, clears its NVRAM failed-start counter to prevent
# a working framebuffer from being disabled after multiple experimental WS restarts.
# Usage: sudo bash xe_gpu_bringup.sh [--status | --no-iop]
# --status: inspect only. --no-iop: leave the experimental display transaction flag off.
set -Eeuo pipefail
umask 022

STATUS_ONLY=0
ENABLE_IOP=1
case "${1:-}" in
    "") ;;
    --status) STATUS_ONLY=1 ;;
    --no-iop) ENABLE_IOP=0 ;;
    *) echo "Usage: sudo bash $0 [--status | --no-iop]" >&2; exit 2 ;;
esac

if [[ "$(uname -s)" != "Darwin" || "$(uname -m)" != "x86_64" ]]; then
    echo "This script must run on an x86_64 macOS installation." >&2
    exit 1
fi
if (( EUID != 0 )) && (( STATUS_ONLY == 0 )); then
    echo "Run with: sudo bash $0" >&2
    exit 1
fi

LOG="/Users/Shared/xe-bringup-$(date '+%Y%m%d-%H%M%S').log"
touch "$LOG" || { echo "Cannot create $LOG" >&2; exit 1; }
exec > >(tee -a "$LOG") 2>&1

stamp() { date '+%H:%M:%S'; }
info() { printf '\n[%s] %s\n' "$(stamp)" "$*"; }
warn() { printf 'WARNING: %s\n' "$*"; }
fail() { printf '\nSTOP: %s\n' "$*"; printf 'Log: %s\n' "$LOG"; exit 1; }

on_exit() {
    local rc=$?
    if (( rc != 0 )); then
        printf '\n[%s] Bring-up stopped (exit %d). No WindowServer restart was performed.\n' "$(stamp)" "$rc"
    fi
    printf 'Log file: %s\n' "$LOG"
}
trap on_exit EXIT

get_sysctl() { sysctl -n "$1" 2>/dev/null; }
loaded() { kmutil showloaded --list-only 2>/dev/null | grep -F "$1" >/dev/null; }
print_sysctl() {
    local key
    for key in "$@"; do
        sysctl "$key" 2>&1 || true
    done
}

show_status() {
    info "Loaded GPU kexts"
    kmutil showloaded --list-only 2>&1 | grep -Ei 'NMIntelFB|XeGfx|NMXAccel' || true
    info "NMIntelFB failed-start guard (read-only)"
    nvram nmintelfb-starts 2>&1 || true
    info "IORegistry services"
    ioreg -p IOService -w0 -t 2>/dev/null | grep -E 'GFX0@2|NMIntelFB|XeGfxResident|NMXAccel|XeGfxAGDC' || true
    info "XeGfx / accelerator / display state"
    print_sysctl \
        debug.xegfx_stage debug.xegfx_running \
        debug.xegfx_agdc debug.xegfx_agdc_fbmap debug.xegfx_agdc_client_ack \
        debug.xegfx_agdc_client_set debug.xegfx_agdc_maxfb \
        debug.nvaccelfb debug.nvaccel_iop \
        debug.nvaccel_heads_published debug.nvaccel_heads_registered debug.nvaccel_heads_piped \
        debug.nvaccel_pipe_init debug.nvaccel_pipe_perform debug.nvaccel_iop_ok_h4 \
        debug.nvaccel_iop_fail debug.nvaccel_scanout_copies
    if get_sysctl debug.xegfx_results >/dev/null; then
        info "XeGfx stage results (indices 0..7; 1=pass, 0=not run, negative=error)"
        sysctl -b debug.xegfx_results | od -An -td4
    fi
}

info "XeGfx GPU bring-up / $(uname -r) / log $LOG"
if (( STATUS_ONLY )); then
    show_status
    exit 0
fi

# Protect the successful working panel. Do not try to reconfigure it here.
info "0: Check existing physical framebuffer"
loaded com.nullmoth.NMIntelFB || fail "NMIntelFB is not loaded. Keep your working desktop; diagnose the auxiliary collection first."
ioreg -p IOService -w0 -t 2>/dev/null | grep -F 'NMIntelFB' >/dev/null || fail "NMIntelFB has no IORegistry service."
if ioreg -p IOService -l -w0 -c NMIntelFB 2>/dev/null | grep -F '"IOFBDependentIndex" = 0' >/dev/null; then
    info "NMIntelFB is in IOGraphics dependent slot 0 (working configuration)."
else
    warn "Could not confirm IOFBDependentIndex=0; verify your corrected NMIntelFB is still installed."
    ioreg -p IOService -l -w0 -c NMIntelFB 2>/dev/null | grep -E 'IOFBDependentID|IOFBDependentIndex' || true
fi

# NMIntelFB counts boots that fail to reach its WindowServerActive callback.
# A working panel can still accumulate that count when an experimental XeGfx/WS
# handoff fails. Clear ONLY after verifying that NMIntelFB is present this boot.
# If the guard already prevented NMIntelFB attaching, stop above rather than
# silently disabling the safety mechanism on a failed boot.
info "0a: Reset NMIntelFB failed-start counter (working framebuffer confirmed)"
if previous_guard="$(nvram nmintelfb-starts 2>/dev/null)"; then
    info "Before reset: $previous_guard"
    nvram -d nmintelfb-starts || fail "Could not clear nmintelfb-starts NVRAM guard."
    if nvram nmintelfb-starts >/dev/null 2>&1; then
        fail "The nmintelfb-starts guard still exists after clearing it."
    fi
    info "Cleared nmintelfb-starts; the next boot will not inherit the prior failure count."
else
    info "nmintelfb-starts is already absent; no reset needed."
fi

# Refuse the older XeGfxAGDC build: its target class is IONDRVFramebuffer,
# and it previously returned -1 after NMIntelFB took ownership of the panel.
AGDC_BIN=/Library/Extensions/XeGfxAGDC.kext/Contents/MacOS/XeGfxAGDC
if [[ ! -f "$AGDC_BIN" ]]; then
    fail "XeGfxAGDC is missing from /Library/Extensions; install the corrected build and rebuild the Auxiliary Kernel Collection."
fi
if ! strings "$AGDC_BIN" | grep -F 'NMIntelFB' >/dev/null; then
    fail "Installed XeGfxAGDC does not contain NMIntelFB. Rebuild with AGDC_FB_CLASS=\"NMIntelFB\", update the auxiliary collection, and reboot."
fi

info "1: Load XeGfx (idle until stages are requested)"
if loaded org.watchthelight.XeGfx; then
    info "XeGfx already loaded."
else
    kmutil load -b org.watchthelight.XeGfx || fail "kmutil could not load org.watchthelight.XeGfx."
fi
get_sysctl debug.xegfx_stage >/dev/null || fail "XeGfx loaded but debug.xegfx_stage is missing. Wrong kext or initialization failure."

info "2: Enable XeGfx stage watchdog (60 seconds)"
sysctl -w debug.xegfx_wdog=60 || fail "Could not set XeGfx watchdog."

# sysctl -w returns success even when a requested stage's *internal* result
# is negative. We MUST read its packed int32 result array as a separate check.
check_stage() {
    local s="$1" state raw value
    local -a results=()
    state="$(get_sysctl debug.xegfx_stage)" || fail "Cannot read debug.xegfx_stage."
    [[ "$state" =~ ^[0-9]+$ ]] || fail "Unexpected stage value: $state"
    raw="$(sysctl -b debug.xegfx_results | od -An -td4 | tr '\n' ' ')" || fail "Cannot read stage results."
    read -r -a results <<< "$raw"
    if [[ "${results[$s]:-}" != "1" ]]; then
        info "Stage results: ${results[*]}"
        sysctl -b debug.xegfx_log | tail -n 100 || true
        fail "XeGfx stage $s did not pass (result=${results[$s]:-missing})."
    fi
    (( state >= s )) || fail "Stage $s reports success, but highest completed stage is $state."
    info "Stage $s passed."
}

run_stage() {
    local stage="$1" description="$2" current
    info "Stage $stage: $description"
    current="$(get_sysctl debug.xegfx_stage)" || fail "Cannot read current stage."
    if (( current < stage )); then
        sysctl -w "debug.xegfx_stage=$stage" || fail "Could not request stage $stage."
    else
        info "Stage $stage already completed; verifying instead of running twice."
    fi
    check_stage "$stage"
}

run_stage 1 "LinuxKPI/XNU shim and VM self-test"
run_stage 2 "Read-only PCI/MMIO/GGTT/firmware-panel probe"
run_stage 3 "PCI newbus + DMA initialization"
run_stage 4 "DRM/i915 + GuC firmware and submission initialization"
run_stage 5 "GPU engine command execution"
run_stage 6 "GPU writeback verification"

info "3: GuC and stage-6 diagnostics"
ioreg -p IOService -l -w0 -c XeGfxResident 2>/dev/null | \
    grep -E -A 18 '"XeGfxGt"|"XeGfxExec"|"XeGfxStore"' | head -n 65 || true

info "4: Load XeGfxAccel and claim the GPU with XeGfx stage 7"
if ! loaded org.watchthelight.XeGfxAccel; then
    kmutil load -b org.watchthelight.XeGfxAccel || fail "Could not load XeGfxAccel (is it installed in the auxiliary collection?)."
fi
run_stage 7 "Set nmxgpu-claimed and trigger XeGfxAccel PCI matching"

# The display head is 4 (bit mask 16), independent of IOFBDependentIndex 0.
info "5: Wait for NMXAccel to publish Intel display head 4"
head_published=0
for ((i=0; i<30; i++)); do
    value="$(get_sysctl debug.nvaccel_heads_published || echo 0)"
    if [[ "$value" =~ ^[0-9]+$ ]] && (( (value & 16) != 0 )); then
        head_published=1
        break
    fi
    sleep 1
done
(( head_published )) || fail "Accelerator did not publish head 4; inspect XeGfxAccel and NMIntelFB matching."
info "Head-4 published mask: $value"

info "6: Register NMIntelFB and enable the scanout-backed display-pipe gate (nvaccelfb=3)"
# 1 registers the framebuffer; 3 also enables gNvScanout and re-delivers mode change.
# The sysctl read-back typically shows 1 (armed), NOT the last value written.
sysctl -w debug.nvaccelfb=3 || fail "Framebuffer registration/scanout sysctl failed."
registered=0
for ((i=0; i<15; i++)); do
    reg="$(get_sysctl debug.nvaccel_heads_registered || echo 0)"
    pipe="$(get_sysctl debug.nvaccel_heads_piped || echo 0)"
    if [[ "$reg" =~ ^[0-9]+$ && "$pipe" =~ ^[0-9]+$ ]] && \
       (( (reg & 16) != 0 && (pipe & 16) != 0 )); then
        registered=1
        break
    fi
    sleep 1
done
(( registered )) || fail "Head 4 is not BOTH registered and piped (registered=${reg:-?}, piped=${pipe:-?}). Do not restart WindowServer."
info "Head 4 successfully registered and piped (registered=$reg, piped=$pipe)."
info "Note: debug.nvaccelfb reads the armed state (usually 1), even after writing 3."

info "7: Load XeGfxAGDC and attach it to Intel PCI + NMIntelFB"
if loaded org.watchthelight.XeGfxAGDC; then
    info "XeGfxAGDC already loaded."
else
    kmutil load -b org.watchthelight.XeGfxAGDC || fail "Could not load XeGfxAGDC."
fi

# AGDC worker is asynchronous. A previous -1 is retryable by writing 1.
agdc_ok=0
for attempt in 1 2 3; do
    info "AGDC start attempt $attempt/3"
    sysctl -w debug.xegfx_agdc=1 || fail "AGDC sysctl failed."
    for ((i=0; i<25; i++)); do
        agdc="$(get_sysctl debug.xegfx_agdc || echo missing)"
        [[ "$agdc" == 1 ]] && { agdc_ok=1; break; }
        [[ "$agdc" == -* ]] && break
        sleep 0.2
    done
    (( agdc_ok )) && break
    sleep 1
done
(( agdc_ok )) || fail "AGDC never reached 1 (last=${agdc:-?}). Usually AGDC_FB_CLASS/PCI provider matching; do not restart WindowServer."

fbmap="$(get_sysctl debug.xegfx_agdc_fbmap || echo missing)"
info "AGDC framebuffer map: $fbmap"
case "$fbmap" in
    'n=1 [0]'*) info "AGDC recognizes exactly one framebuffer at index 0." ;;
    *) fail "AGDC does not report NMIntelFB in slot 0 (map=$fbmap); do not restart WindowServer." ;;
esac

# XeGfx builds default client_ack to 1; confirm it, and set it if necessary.
ack="$(get_sysctl debug.xegfx_agdc_client_ack || echo missing)"
if [[ "$ack" == 0 ]]; then
    sysctl -w debug.xegfx_agdc_client_ack=1 || fail "AGDC client ACK could not be enabled."
elif [[ "$ack" != 1 ]]; then
    fail "AGDC client ACK sysctl is unexpectedly $ack."
fi

if (( ENABLE_IOP )); then
    info "8: Advertise experimental display-pipe transaction support (no WindowServer restart)"
    sysctl -w debug.nvaccel_iop=1 || fail "Could not set debug.nvaccel_iop=1."
else
    info "8: Skipping experimental IOP transaction gate (--no-iop)."
fi

show_status
info "READY FOR MANUAL TEST. This script DID NOT restart WindowServer."
info "Verify the Metal plugin and Mesa/ANV are installed before expecting acceleration."
info "To restart manually ONLY when ready: sudo killall -TERM WindowServer"
info "If the display goes black, inspect log/counters over SSH; a reboot clears the manually started XeGfx stack."
