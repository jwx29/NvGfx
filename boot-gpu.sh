#!/bin/bash
# XeGfx manual per-boot bring-up for macOS Sequoia / Intel Meteor Lake.
# Uses installed Auxiliary Kernel Collection kexts; does not install or rebuild.
# Does NOT restart WindowServer, enable startup automation, or change OpenCore.
# On a healthy NMIntelFB boot, clears its NVRAM failed-start counter to prevent
# a working framebuffer from being disabled after multiple experimental WS restarts.
# Usage: sudo bash xe_gpu_bringup_with_tests.sh [--status] [--no-iop] [--no-tests]
#        sudo bash xe_gpu_bringup_with_tests.sh --tests-only [--full-tests]
# --status: inspect only. --no-iop: leave experimental display transactions off.
# --no-tests: bring up the stack without running userland tests.
# --tests-only: rerun diagnostics on an already-initialised stack.
# --full-tests: run upstream's entire metal_selftest suite instead of its smoke subset.
# Kernel stage 1..7 already run as part of bring-up; xgprobe and Metal tests follow.
# Userland checks run as the logged-in user (not root) and never restart WindowServer.
set -Eeuo pipefail
umask 022

STATUS_ONLY=0
ENABLE_IOP=1
RUN_TESTS=1
TESTS_ONLY=0
FULL_TESTS=0
for arg in "$@"; do
    case "$arg" in
        --status) STATUS_ONLY=1 ;;
        --no-iop) ENABLE_IOP=0 ;;
        --no-tests) RUN_TESTS=0 ;;
        --tests-only) TESTS_ONLY=1 ;;
        --full-tests) FULL_TESTS=1 ;;
        -h|--help)
            echo "Usage: sudo bash $0 [--status] [--no-iop] [--no-tests] [--full-tests]"
            echo "       sudo bash $0 --tests-only [--full-tests]"
            exit 0 ;;
        *) echo "Unknown option: $arg" >&2; exit 2 ;;
    esac
done
if (( STATUS_ONLY && (TESTS_ONLY || FULL_TESTS) )); then
    echo "--status cannot be combined with test options" >&2; exit 2
fi
if (( TESTS_ONLY && !RUN_TESTS )); then
    echo "--tests-only conflicts with --no-tests" >&2; exit 2
fi
if (( FULL_TESTS && !RUN_TESTS )); then
    echo "--full-tests conflicts with --no-tests" >&2; exit 2
fi

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
        printf '\n[%s] Bring-up or validation ended with exit %d. No WindowServer restart was performed.\n' "$(stamp)" "$rc"
    fi
    printf 'Log file: %s\n' "$LOG"
}
trap on_exit EXIT

get_sysctl() { sysctl -n "$1" 2>/dev/null; }
# Match the COMPLETE bundle identifier, not a substring. For example, a
# loaded XeGfxAccel or XeGfxAGDC must never be mistaken for XeGfx.
# kmutil's table usually prints "bundle.id (version)"; --list-only may
# print just the ID. Inspect every whitespace-delimited field in either case.
loaded() {
    local bundle="$1"
    kmutil showloaded --list-only 2>/dev/null | awk -v want="$bundle" '
        {
            for (i = 1; i <= NF; i++) {
                token = $i
                sub(/\(.*/, "", token)
                if (token == want) found = 1
            }
        }
        END { exit(found ? 0 : 1) }
    '
}

# XeGfx _start registers this sysctl and publishes XeGfxArm.
# It is a better proof of initialization than a kmutil listing alone.
ensure_xegfx_started() {
    if get_sysctl debug.xegfx_stage >/dev/null; then
        info "XeGfx is already initialised (debug.xegfx_stage exists)."
        return 0
    fi
    if loaded org.watchthelight.XeGfx; then
        warn "XeGfx appears in kmutil but its stage sysctl is absent; attempting explicit kmutil load."
    else
        info "XeGfx not currently loaded; requesting explicit load."
    fi
    kmutil load -b org.watchthelight.XeGfx || \
        fail "kmutil load failed for XeGfx. Check the installed kext, auxiliary collection and kmutil error above."
    local i
    for ((i=0; i<20; i++)); do
        if get_sysctl debug.xegfx_stage >/dev/null; then
            info "XeGfx started; debug.xegfx_stage is available."
            return 0
        fi
        sleep 0.25
    done
    fail "XeGfx is listed/loaded, but debug.xegfx_stage is missing. Its _start did not complete or an old/wrong kext is installed; do not run GPU stages."
}

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


# ---- Post-bring-up verification ----
# XeGfx stages 1..7 are in-kernel tests. The upstream xgprobe tests /dev/dri
# ioctls, contexts and GPU readbacks. NvGfx tools/metal_selftest.sh tests
# Metal-on-ANV userspace; it MUST run in a regular login-user context.
#
# Missing external test tools or a missing plugin are INCOMPLETE (not PASS).
# Userland tests can still hang/crash this experimental GPU. Neither this
# function nor any other path in this script restarts WindowServer.
TEST_PASS=0
TEST_FAIL=0
TEST_SKIP=0
TEST_DIR=""

test_result() {
    local label="$1" outcome="$2" detail="${3:-}"
    printf '[TEST] %-26s %-10s %s\n' "$label" "$outcome" "$detail"
    case "$outcome" in
        PASS) TEST_PASS=$((TEST_PASS + 1)) ;;
        FAIL|TIMEOUT) TEST_FAIL=$((TEST_FAIL + 1)) ;;
        SKIP) TEST_SKIP=$((TEST_SKIP + 1)) ;;
    esac
}

# Run a command as the desktop user with a bounded wall-time.
# start_new_session lets us kill the subprocess group after a timeout.
run_limited_user() {
    local seconds="$1"
    shift
    python3 - "$seconds" "$TEST_USER" "$TEST_HOME" "$@" <<'PY'
import os, signal, subprocess, sys
seconds = int(sys.argv[1])
user = sys.argv[2]
home = sys.argv[3]
argv = sys.argv[4:]
env = os.environ.copy()
env["HOME"] = home
env["USER"] = user
env["LOGNAME"] = user
cmd = ["/usr/bin/sudo", "-n", "-u", user, "/usr/bin/env",
       "HOME=" + home, "USER=" + user, "LOGNAME=" + user] + argv
try:
    p = subprocess.Popen(cmd, start_new_session=True, env=env)
except OSError as exc:
    print("Could not launch test: %s" % exc, flush=True)
    sys.exit(127)
try:
    sys.exit(p.wait(timeout=seconds))
except subprocess.TimeoutExpired:
    try:
        os.killpg(p.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    p.wait()
    print("TEST TIMEOUT after %d seconds" % seconds, flush=True)
    sys.exit(124)
PY
}

# Lookup the login user independently from root's HOME. Do not run the Metal
# tests as root: they must see the user's framework and graphics environment.
choose_test_user() {
    TEST_USER="${SUDO_USER:-}"
    if [[ -z "$TEST_USER" || "$TEST_USER" == "root" ]]; then
        TEST_USER="$(stat -f '%Su' /dev/console 2>/dev/null || true)"
    fi
    if [[ -z "$TEST_USER" || "$TEST_USER" == "root" || "$TEST_USER" == "loginwindow" ]]; then
        return 1
    fi
    TEST_HOME="$(python3 - "$TEST_USER" <<'PY'
import pwd, sys
try:
    print(pwd.getpwnam(sys.argv[1]).pw_dir)
except KeyError:
    sys.exit(1)
PY
)" || return 1
    [[ -d "$TEST_HOME" ]]
}

kernel_test_results() {
    local state raw n
    local -a array=()
    state="$(get_sysctl debug.xegfx_stage 2>/dev/null || true)"
    if [[ ! "$state" =~ ^[0-9]+$ ]] || (( state < 7 )); then
        test_result "XeGfx stages 1-7" FAIL "highest completed stage: ${state:-unavailable}"
        return 1
    fi
    raw="$(sysctl -b debug.xegfx_results 2>/dev/null | od -An -td4 | tr '\n' ' ')" || true
    read -r -a array <<< "$raw"
    for n in 1 2 3 4 5 6 7; do
        if [[ "${array[$n]:-}" != "1" ]]; then
            test_result "XeGfx stages 1-7" FAIL "stage $n result=${array[$n]:-missing}"
            return 1
        fi
    done
    test_result "XeGfx stages 1-7" PASS "all kernel checks successful"
}

accelerator_test_results() {
    local pub reg pipe agdc
    if ! get_sysctl debug.nvaccelfb >/dev/null; then
        test_result "XeGfxAccel attachment" FAIL "debug.nvaccelfb missing"
        return 1
    fi
    pub="$(get_sysctl debug.nvaccel_heads_published || echo '')"
    reg="$(get_sysctl debug.nvaccel_heads_registered || echo '')"
    pipe="$(get_sysctl debug.nvaccel_heads_piped || echo '')"
    agdc="$(get_sysctl debug.xegfx_agdc || echo '')"
    if [[ "$pub" =~ ^[0-9]+$ && "$reg" =~ ^[0-9]+$ && "$pipe" =~ ^[0-9]+$ ]] &&
       (( (pub & 16) && (reg & 16) && (pipe & 16) )); then
        test_result "XeGfxAccel head 4" PASS "published=$pub registered=$reg piped=$pipe"
    else
        test_result "XeGfxAccel head 4" FAIL "published=$pub registered=$reg piped=$pipe"
    fi
    if [[ "$agdc" == "1" ]]; then
        test_result "XeGfxAGDC" PASS "active"
    else
        test_result "XeGfxAGDC" FAIL "state=${agdc:-unavailable}"
    fi
}

run_selftests() {
    info "9: XeGfx/NvGfx self-tests (without restarting WindowServer)"
    TEST_PASS=0; TEST_FAIL=0; TEST_SKIP=0
    TEST_DIR="/Users/Shared/xe-tests-$(date '+%Y%m%d-%H%M%S')-$$"
    mkdir -p "$TEST_DIR" || fail "Cannot create test reports at $TEST_DIR."
    chmod 755 "$TEST_DIR"

    kernel_test_results || true
    accelerator_test_results || true

    if ! choose_test_user; then
        test_result "Userspace test identity" FAIL "no non-root desktop login user found"
    else
        info "Run userspace tests as $TEST_USER ($TEST_HOME), never root."

        # xgprobe is produced by XeGfx's link.sh if i915_drm.h is available.
        # It exercises a real DRM render node, not just kernel startup.
        local probe node probe_rc
        probe=""
        for probe_candidate in \
            "/usr/local/bin/xgprobe" \
            "$TEST_HOME/src/NvGfx/gpu/xegfx/out/xgprobe"; do
            if [[ -x "$probe_candidate" ]]; then
                probe="$probe_candidate"
                break
            fi
        done
        node="${XEGFX_RENDER_NODE:-/dev/dri/renderD128}"
        if [[ -z "$probe" ]]; then
            test_result "XeGfx xgprobe" SKIP "binary missing; build with i915_drm.h available"
        elif [[ ! -e "$node" ]]; then
            test_result "XeGfx xgprobe" FAIL "render node missing: $node"
        else
            info "xgprobe: $probe $node (45s time limit)"
            if run_limited_user 45 "$probe" "$node" >"$TEST_DIR/xgprobe.log" 2>&1; then
                probe_rc=0
            else
                probe_rc=$?
            fi
            tail -n 30 "$TEST_DIR/xgprobe.log" || true
            if (( probe_rc == 0 )); then
                test_result "XeGfx xgprobe" PASS "GPU context and ring readbacks"
            elif (( probe_rc == 124 )); then
                test_result "XeGfx xgprobe" TIMEOUT "see $TEST_DIR/xgprobe.log"
            else
                test_result "XeGfx xgprobe" FAIL "exit=$probe_rc; see $TEST_DIR/xgprobe.log"
            fi
        fi

        # Upstream Metal suite compiles its Objective-C tests in /tmp and
        # internally times every test. Running as the desktop user avoids
        # root-vs-user Metal device selection and permissions differences.
        local repo mtest plugin metal_rc limit
        repo="${NVGFX_REPO:-$TEST_HOME/src/NvGfx}"
        mtest="$repo/tools/metal_selftest.sh"
        plugin="/Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver"
        if [[ ! -f "$mtest" ]]; then
            test_result "NvGfx Metal selftests" SKIP "not found: $mtest"
        elif [[ ! -f "$plugin" ]]; then
            test_result "NvGfx Metal selftests" SKIP "NVMTLDriver.bundle not installed"
        else
            limit=120
            info "NvGfx Metal: $( (( FULL_TESTS )) && echo 'full upstream suite' || echo 'pipeline, surface and ordering smoke suite')"
            info "Individual test timeout capped to ${limit}s; output -> $TEST_DIR/metal.log"
            local -a filters=()
            if (( !FULL_TESTS )); then
                filters=("pipeline-build" "surface-wrappers" "surface-order queued")
            fi
            # Upstream suite has per-check timeouts. A second overall
            # watchdog bounds the whole suite in case a compiler hangs.
            # Full suite may contain 17 checks; allow up to 40 minutes.
            local max_wall
            if (( FULL_TESTS )); then max_wall=2400; else max_wall=480; fi
            if run_limited_user "$max_wall" \
                 /usr/bin/env "SELFTEST_MAX=$limit" \
                 /bin/bash "$mtest" "${filters[@]}" >"$TEST_DIR/metal.log" 2>&1; then
                metal_rc=0
            else
                metal_rc=$?
            fi
            cat "$TEST_DIR/metal.log"
            # An entirely skipped suite is not proof that Metal works.
            # Also reject tests against a missing default Metal device.
            local metal_device metal_skipped
            metal_device="$(sed -n 's/^Metal[[:space:]]*//p' "$TEST_DIR/metal.log" | head -n 1)"
            metal_skipped="$(sed -nE 's/^[0-9]+ ran, [0-9]+ failed, ([0-9]+) skipped.*/\1/p' "$TEST_DIR/metal.log" | tail -n 1)"
            if [[ "$metal_device" == "(no Metal device)" || -z "$metal_device" ]]; then
                test_result "NvGfx Metal selftests" FAIL "no default Metal device; check plugin and WindowServer state"
            elif [[ "$metal_device" != *Intel* && "$metal_device" != *Xe* ]]; then
                test_result "NvGfx Metal selftests" FAIL "tests used another Metal device: $metal_device"
            elif (( metal_rc == 124 )); then
                test_result "NvGfx Metal selftests" TIMEOUT "see $TEST_DIR/metal.log"
            elif grep -qE '^[0-9]+ ran, ' "$TEST_DIR/metal.log" &&
                 grep -qE '^[1-9][0-9]* ran, 0 failed, ' "$TEST_DIR/metal.log" &&
                 (( metal_rc == 0 )); then
                test_result "NvGfx Metal selftests" PASS "on $metal_device"
                if [[ "$metal_skipped" =~ ^[0-9]+$ ]] && (( metal_skipped > 0 )); then
                    test_result "Metal checks not present" SKIP "$metal_skipped source check(s) unavailable in this checkout"
                fi
            else
                test_result "NvGfx Metal selftests" FAIL "exit=$metal_rc; see $TEST_DIR/metal.log"
            fi
        fi
    fi

    printf '\nSelf-test summary: %d PASS, %d FAIL, %d SKIP\n' \
        "$TEST_PASS" "$TEST_FAIL" "$TEST_SKIP"
    info "Individual test reports: $TEST_DIR"
    if (( TEST_FAIL > 0 )); then
        warn "One or more self-tests failed. Do NOT restart WindowServer on this evidence."
        return 1
    fi
    if (( TEST_SKIP > 0 )); then
        warn "Checks were skipped: kernel readiness is not proof of working Vulkan/Metal."
        return 2
    fi
    info "Self-tests passed. This does not prove long-term WindowServer stability."
    return 0
}

info "XeGfx GPU bring-up / $(uname -r) / log $LOG"
if (( STATUS_ONLY )); then
    show_status
    exit 0
fi
if (( TESTS_ONLY )); then
    run_selftests
    exit $?
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

info "1: Start XeGfx and verify the stage interface"
ensure_xegfx_started

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
if loaded org.watchthelight.XeGfxAccel; then
    info "XeGfxAccel kext is already loaded; its PCI personality may still be unmatched."
else
    info "Loading XeGfxAccel kext explicitly."
    kmutil load -b org.watchthelight.XeGfxAccel || fail "Could not load XeGfxAccel (is it installed in the auxiliary collection?)."
fi
# This stage publishes nmxgpu-claimed, causing accelerator PCI matching.
# debug.nvaccelfb need not exist BEFORE stage 7; it is installed when
# the accelerator's IOService starts, rather than when the kext is merely loaded.
run_stage 7 "Set nmxgpu-claimed and trigger XeGfxAccel PCI matching"
for ((i=0; i<20; i++)); do
    if get_sysctl debug.nvaccelfb >/dev/null; then break; fi
    sleep 0.25
done
get_sysctl debug.nvaccelfb >/dev/null || \
    fail "Stage 7 ran, but XeGfxAccel's debug.nvaccelfb is absent. Kext present != accelerator matched; inspect XeGfxAccel IORegistry personality and nmxgpu-claimed."

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
    info "XeGfxAGDC kext is listed as loaded (exact bundle ID)."
else
    info "Loading XeGfxAGDC explicitly."
    kmutil load -b org.watchthelight.XeGfxAGDC || fail "Could not load XeGfxAGDC."
fi
if ! get_sysctl debug.xegfx_agdc >/dev/null; then
    warn "debug.xegfx_agdc is not yet registered. The AGDC service may not be attached."
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
if (( RUN_TESTS )); then
    run_selftests
else
    info "Self-tests skipped by --no-tests."
fi

sudo sysctl -w debug.xegfx_agdc_k5=1

info "READY FOR MANUAL TEST. This script DID NOT restart WindowServer."
info "Verify the Metal plugin and Mesa/ANV are installed before expecting acceleration."
info "To restart manually ONLY when ready: sudo killall -TERM WindowServer"
info "If the display goes black, inspect log/counters over SSH; a reboot clears the manually started XeGfx stack."
