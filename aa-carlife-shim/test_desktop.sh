#!/usr/bin/env bash
# End-to-end test of the built armhf binaries (out/*.armhf), run under qemu:
#   docker run --rm --platform linux/arm/v7 -v "$PWD":/w debian:bookworm bash /w/aa-carlife-shim/test_desktop.sh
#
# What it proves: our shim and our client agree with each other over real RCF on TCP
# (service, publish/subscribe, registry file, control socket, procguard-style arguments).
# What it cannot prove: compatibility with the stock RCF 2.2 peers. That is the unit test
# (README "Step 1"); the method-ID table check in build.sh is the static half of it.
set -uo pipefail

OUT="$(cd "$(dirname "$0")" && pwd)/out"
SHIM="${SHIM:-${OUT}/aa-carlife-shim.armhf}"
PROBE="${OUT}/cl_probe.armhf"
FAKE="${OUT}/fake_receiver.armhf"
FAKE_ILM="${OUT}/fake_ilm.armhf"
FAKE_SOURCE="${OUT}/fake_source.armhf"
FAKE_CAN="${OUT}/fake_can.armhf"
FAKE_BT="${OUT}/fake_bt.armhf"
FAKE_POWER="${OUT}/fake_power.armhf"
FAKE_SYSLOG="${OUT}/fake_syslog.armhf"
T=/tmp/aa-shim-test
rm -rf "${T}" && mkdir -p "${T}"

# QEMU_SYSROOT=<device sysroot>: run every binary on the unit's own loader and glibc 2.15
# through qemu-arm -L (from an arm64 container with qemu-user installed), instead of the
# container's armhf libc. exec keeps the PID, so the SIGTERM tests still reach the shim.
if [ -n "${QEMU_SYSROOT:-}" ]; then
    mkdir -p "${T}/bin"
    for b in SHIM PROBE FAKE FAKE_ILM FAKE_SOURCE FAKE_CAN FAKE_BT FAKE_POWER FAKE_SYSLOG; do
        w="${T}/bin/$(basename "${!b}")"
        printf '#!/bin/sh\nexec qemu-arm -L "%s" "%s" "$@"\n' "${QEMU_SYSROOT}" "${!b}" > "${w}"
        chmod +x "${w}"
        printf -v "${b}" '%s' "${w}"
    done
    echo "(running on the device loader and libc from ${QEMU_SYSROOT})"
fi

export AA_CL_REGISTRY_DIR="${T}/registry"
REG="${AA_CL_REGISTRY_DIR}/svp.svc.carlife"
# The launcher's files, here instead of /tmp/run/aa (the shim makes the directory).
export AA_SHIM_FATAL_FILE="${T}/run-aa/shim.fatal"
export AA_SHIM_NOTIFY="${T}/run-aa/notify.sock"

fails=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }
# $3: a log to show (its end) when the check fails.
check() {
    if eval "$2"; then pass "$1"; else
        fail "$1"
        if [ -n "${3:-}" ] && [ -f "$3" ]; then tail -n 25 "$3" | sed 's/^/    | /'; fi
    fi
}

# Wait up to $3 s (default 5) for at least $4 (default 1) lines matching pattern $2 in file $1.
wait_log() {
    for _ in $(seq 1 $(( ${3:-5} * 10 ))); do
        [ "$(grep -c -- "$2" "$1" 2>/dev/null)" -ge "${4:-1}" ] && return 0
        sleep 0.1
    done
    return 1
}

wait_for_file() {
    for _ in $(seq 1 100); do [ -e "$1" ] && return 0; sleep 0.1; done
    return 1
}

# Status lines from the listener, in order, as "1 2 3 ...".
statuses() { sed -n 's/.*OnCarLifeConnectStatusChange(\([0-9]*\), .*/\1/p' "$1" | tr '\n' ' ' | sed 's/ $//'; }

start_shim() {   # $1 = log file, rest = extra env
    local log="$1"; shift
    # --debug-output console: the log to stderr (any other mode goes to syslog, section 22).
    env "$@" "${SHIM}" --debug-output console --bogus-flag > "${log}" 2>&1 &
    SHIM_PID=$!
    wait_for_file "${REG}" || { fail "shim did not register"; cat "${log}"; exit 1; }
}

stop_shim() {
    kill -TERM "${SHIM_PID}"
    wait "${SHIM_PID}"
    SHIM_RC=$?
}

echo "=== 1. fake mode: connect plays 2 (connecting) -> 3 (video) without a receiver"
start_shim "${T}/shim1.log" AA_SHIM_FAKE=1 AA_SHIM_VERBOSE=1
check "registry file created" "[ -e '${REG}' ]"
check "the start line names the build" "grep -q 'starting: build src-[0-9a-f]\{8\}' '${T}/shim1.log'"
check "unknown argument only warns" "grep -q 'ignoring unknown argument --bogus-flag' '${T}/shim1.log'"

"${PROBE}" listen 6 > "${T}/listen1.log" 2>&1 &
LISTEN_PID=$!
sleep 1
check "status before connect is 0" "[ \"\$('${PROBE}' status)\" = 'GetCarLifeConnectStatus() = 0' ]"
# Stock clients (libcarlife.so) dial 0.0.0.0:30407; Linux routes that to loopback.
check "a client dialing 0.0.0.0 reaches the loopback-bound shim" \
      "[ \"\$('${PROBE}' --host 0.0.0.0 status)\" = 'GetCarLifeConnectStatus() = 0' ]"
"${PROBE}" ro > "${T}/ro1.log" 2>&1
check "all getters answer" "[ \$(wc -l < '${T}/ro1.log') -eq 11 ] && grep -q 'ConnectCarlifePhoneIsiOS() = 0' '${T}/ro1.log'"
check "write commands refused without --allow-write" "! '${PROBE}' connect 2>/dev/null"
"${PROBE}" --allow-write connect > /dev/null
sleep 3
check "status after connect is 3 (video)" "[ \"\$('${PROBE}' status)\" = 'GetCarLifeConnectStatus() = 3' ]"
# connectionservice calls CarLifeConnection() again when the phone re-attaches (for example
# after the receiver's accessory-mode switch); during a session that changes nothing, but it
# is answered with 2 then 3 again, so appconnect's "connecting" for that call ends (run 45).
"${PROBE}" --allow-write connect > /dev/null
sleep 0.6
check "a second CarLifeConnection() during a session: announced again, the session untouched" \
      "grep -q 'session running; announcing it again' '${T}/shim1.log' && [ \"\$('${PROBE}' status)\" = 'GetCarLifeConnectStatus() = 3' ]"
"${PROBE}" --allow-write touch down 100 200 > /dev/null
"${PROBE}" --allow-write rotate 0x3f0 1 > /dev/null
"${PROBE}" --allow-write disconnect > /dev/null
wait "${LISTEN_PID}"
check "listener saw 2 3, 2 3 (announced again), 4" "[ \"\$(statuses '${T}/listen1.log')\" = '2 3 2 3 4' ]"
check "no progress events (stock sends none on connect)" "! grep -q OnConnectingStatusPercentage '${T}/listen1.log'"
check "shim logged the touch" "grep -q 'SingleTouchInfo(100, 200, down)' '${T}/shim1.log'"
check "shim logged the rotary key" "grep -q 'RotateKeyEvent(0x03f0, 1)' '${T}/shim1.log'"
stop_shim
check "SIGTERM exits 0" "[ ${SHIM_RC} -eq 0 ]"
check "registry file removed on exit" "[ ! -e '${REG}' ]"

echo "=== 1b. a client that subscribes while a session runs (appconnect starting late) hears it: 2 3"
start_shim "${T}/shim1b.log" AA_SHIM_FAKE=1 AA_SHIM_FAKE_VIDEO_MS=300
"${PROBE}" --allow-write connect > /dev/null
sleep 1.5
"${PROBE}" listen 3 > "${T}/listen1b.log" 2>&1
check "the late subscriber got 2 then 3, and the voice module's status" \
      "[ \"\$(statuses '${T}/listen1b.log')\" = '2 3' ] && grep -q '8 OnModuleStatusChange(4, 0)' '${T}/listen1b.log'" "${T}/shim1b.log"
"${PROBE}" --allow-write disconnect > /dev/null
"${PROBE}" listen 2 > "${T}/listen1c.log" 2>&1
check "no session: a new subscriber hears nothing" "[ -z \"\$(statuses '${T}/listen1c.log')\" ]" "${T}/listen1c.log"
stop_shim

echo "=== 2. receiver mode: shim drives the receiver over the control socket"
CTL="${T}/ctl.sock"
start_shim "${T}/shim2.log" AA_CTL_SOCK="${CTL}"
"${FAKE}" --sock "${CTL}" --step-ms 200 connected projecting > "${T}/fake2.log" 2>&1 &
FAKE_PID=$!
"${PROBE}" listen 5 > "${T}/listen2.log" 2>&1 &
LISTEN_PID=$!
sleep 1
"${PROBE}" --allow-write connect > /dev/null
sleep 1.5
check "status is 3 (video) after the receiver reports projecting" "[ \"\$('${PROBE}' status)\" = 'GetCarLifeConnectStatus() = 3' ]"
"${PROBE}" --allow-write touch move 10 20 > /dev/null
"${PROBE}" --allow-write play 1 > /dev/null
"${PROBE}" --allow-write disconnect > /dev/null
wait "${LISTEN_PID}"
check "receiver got start" "grep -q \"got 'start'\" '${T}/fake2.log'"
check "receiver got the touch" "grep -q \"got 'touch move 10 20'\" '${T}/fake2.log'"
check "receiver got play as a keyfeed token" "grep -q \"got 'key play'\" '${T}/fake2.log'"
check "receiver got stop" "grep -q \"got 'stop'\" '${T}/fake2.log'"
check "listener saw 2 3 4" "[ \"\$(statuses '${T}/listen2.log')\" = '2 3 4' ]"
stop_shim
wait "${FAKE_PID}" 2>/dev/null

echo "=== 3. receiver missing: connect fails with (1, 1), as stock does when ConnectionInit fails"
start_shim "${T}/shim3.log" AA_CTL_SOCK="${CTL}" AA_SHIM_RECEIVER_WAIT_MS=800
"${PROBE}" listen 3 > "${T}/listen3.log" 2>&1 &
LISTEN_PID=$!
sleep 1
"${PROBE}" --allow-write connect > /dev/null
wait "${LISTEN_PID}"
check "listener saw 2 1, the failure with error 1" "[ \"\$(statuses '${T}/listen3.log')\" = '2 1' ] && grep -q 'OnCarLifeConnectStatusChange(1, 1)' '${T}/listen3.log'"
stop_shim

echo "=== 4. receiver never shows video: connect times out with (5, 1) and stop"
start_shim "${T}/shim4.log" AA_CTL_SOCK="${CTL}" AA_SHIM_CONNECT_TIMEOUT_MS=1000
"${FAKE}" --sock "${CTL}" --step-ms 100 connected > "${T}/fake4.log" 2>&1 &
FAKE_PID=$!
"${PROBE}" listen 3 > "${T}/listen4.log" 2>&1 &
LISTEN_PID=$!
sleep 1
"${PROBE}" --allow-write connect > /dev/null
wait "${LISTEN_PID}"
check "listener saw 2 5, the timeout with error 1" "[ \"\$(statuses '${T}/listen4.log')\" = '2 5' ] && grep -q 'OnCarLifeConnectStatusChange(5, 1)' '${T}/listen4.log'"
check "receiver was told to stop" "grep -q \"got 'stop'\" '${T}/fake4.log'"
stop_shim
wait "${FAKE_PID}" 2>/dev/null

echo "=== 5. port already taken: start-up fails with exit 1 (procguard leaves it down)"
start_shim "${T}/shim5a.log" AA_SHIM_FAKE=1
FIRST=${SHIM_PID}
AA_SHIM_FAKE=1 AA_CL_REGISTRY_DIR="${T}/registry2" "${SHIM}" > "${T}/shim5b.log" 2>&1
check "second instance exits 1" "[ $? -eq 1 ]"
check "second instance did not register" "[ ! -e '${T}/registry2/svp.svc.carlife' ]"
check "nor asked the launcher for the stock service (the first one serves the slot)" "[ ! -e '${AA_SHIM_FATAL_FILE}' ]"
check "first instance still answers" "[ \"\$('${PROBE}' status)\" = 'GetCarLifeConnectStatus() = 0' ]"
SHIM_PID=${FIRST}
stop_shim

echo "=== 5b. any other start-up failure: the reason for the launcher, the stale registry file gone, exit 0"
mkdir -p "${AA_CL_REGISTRY_DIR}" && : > "${REG}"
: > "${T}/notadir"
AA_CTL_SOCK="${T}/notadir/ctl.sock" "${SHIM}" > "${T}/shim5c.log" 2>&1
RC5=$?
check "exit 0, so procguard runs the launcher again" "[ ${RC5} -eq 0 ]" "${T}/shim5c.log"
check "the reason is in AA_SHIM_FATAL_FILE (its directory made)" "grep -q 'control socket' '${AA_SHIM_FATAL_FILE}'" "${T}/shim5c.log"
check "the stale registry file is gone (clients do not wait on a dead service)" "[ ! -e '${REG}' ]"
rm -f "${AA_SHIM_FATAL_FILE}"

echo "=== 6. stale registry file (a killed carlifeservice left it): deleted, then re-created"
mkdir -p "${AA_CL_REGISTRY_DIR}" && : > "${REG}"
AA_SHIM_FAKE=1 "${SHIM}" > "${T}/shim6.log" 2>&1 &
SHIM_PID=$!
for _ in $(seq 1 100); do grep -q 'INFO: ready' "${T}/shim6.log" && break; sleep 0.1; done
check "stale file removed first (clients see IN_DELETE then IN_CREATE)" \
      "grep -q 'removed a stale' '${T}/shim6.log' && grep -q 'INFO: registered' '${T}/shim6.log'"
check "registry file present after start" "[ -e '${REG}' ]"
stop_shim

echo "=== 7. the real receiver (gated) against the shim: start arms it, the shim's stop disarms it"
# Static musl binary: it runs the same with or without QEMU_SYSROOT. No phone and no usbfs
# here, so this covers the control link only (connect, start/stop, shim going away).
RECV="$(cd "$(dirname "$0")/.." && pwd)/aa-headless.armhf"
if [ -x "${RECV}" ]; then
    CTL7="${T}/ctl7.sock"
    start_shim "${T}/shim7.log" AA_CTL_SOCK="${CTL7}" AA_SHIM_CONNECT_TIMEOUT_MS=3000
    AA_CTL_SOCK="${CTL7}" AA_SESSIONS_DIR="${T}/sess7" AA_VIDEO_SOCK="${T}/video7.sock" AA_MIC_SOCK="${T}/mic7.sock" \
        "${RECV}" --debug-output none > "${T}/recv7.log" 2>&1 &
    RECV_PID=$!
    for _ in $(seq 1 50); do grep -q 'connected to the shim' "${T}/recv7.log" && break; sleep 0.2; done
    check "receiver prints its gated config" "grep -q 'gated: switch only after' '${T}/recv7.log'"
    check "receiver connects to the shim's control socket" "grep -q 'connected to the shim' '${T}/recv7.log'"
    "${PROBE}" listen 6 > "${T}/listen7.log" 2>&1 &
    LISTEN_PID=$!
    sleep 1
    "${PROBE}" --allow-write connect > /dev/null
    sleep 1
    check "the shim's start arms the receiver" "grep -q 'start: armed' '${T}/recv7.log'"
    "${PROBE}" --allow-write touch down 400 240 > /dev/null   # no session yet: dropped quietly
    wait "${LISTEN_PID}"
    check "no phone: the shim times out with (5, 1) and stops the receiver" \
          "[ \"\$(statuses '${T}/listen7.log')\" = '2 5' ] && grep -q 'stop: disarmed' '${T}/recv7.log'"
    stop_shim
    sleep 1
    check "shim gone: the receiver disarms and keeps running" \
          "grep -q 'shim gone: disarming' '${T}/recv7.log' && kill -0 ${RECV_PID}"
    kill "${RECV_PID}" 2>/dev/null; wait "${RECV_PID}" 2>/dev/null
else
    fail "receiver binary ${RECV} missing (run r2_musl.sh)"
fi

echo "=== 8. Show(n) drives the iLM service on the bridge's surface (20000000 + bridge pid)"
"${FAKE_ILM}" --port 31005 > "${T}/ilm8.log" 2>&1 &
ILM_PID=$!
# A stand-in bridge: any process named aa-bridge (one of the shim's AA_BRIDGE_COMM names).
cp "$(command -v sleep)" "${T}/aa-bridge"
"${T}/aa-bridge" 120 &
BRIDGE_PID=$!
SURF=$((20000000 + BRIDGE_PID))
CTL8="${T}/ctl8.sock"
start_shim "${T}/shim8.log" AA_ILM_PORT=31005 AA_CTL_SOCK="${CTL8}"
sleep 0.5
"${PROBE}" --allow-write show 1 > /dev/null
sleep 4
check "Show(1): WaitSurface, ShowSurface(${SURF}, 1), and once more 3 s later" \
      "grep -q 'WaitSurface(${SURF})' '${T}/ilm8.log' && [ \$(grep -c 'ShowSurface(${SURF}, 1)' '${T}/ilm8.log') -eq 2 ]" "${T}/shim8.log"
# The crop (source/destination rectangles) goes through the unit's libilmControl. Debian has
# none; the device sysroot (QEMU_SYSROOT) has it, but ilm_init finds no compositor here. Either
# way the shim must try once, say so, and carry on with the shows.
check "libilmControl missing or not initialised: tried once, logged, shows still sent" \
      "[ \$(grep -cE 'libilmControl not available|ilm_init -> [1-9]' '${T}/shim8.log') -eq 1 ]" "${T}/shim8.log"
# A new AA session (receiver: projecting) while the screen is up shows the new surface again.
"${FAKE}" --sock "${CTL8}" --step-ms 100 connected projecting > "${T}/fake8.log" 2>&1 &
FAKE_PID=$!
sleep 0.5
"${PROBE}" --allow-write connect > /dev/null
sleep 1.5
check "new session while shown: shown again" "[ \$(grep -c 'ShowSurface(${SURF}, 1)' '${T}/ilm8.log') -ge 3 ]" "${T}/ilm8.log"
# procguard restarts a bridge that died: a new pid, so a new surface, shown with no Show(1).
kill "${BRIDGE_PID}" 2>/dev/null; wait "${BRIDGE_PID}" 2>/dev/null
"${T}/aa-bridge" 120 &
BRIDGE_PID=$!
SURF=$((20000000 + BRIDGE_PID))
sleep 6
check "the bridge restarted while shown: its new surface ${SURF} shown (looked for every 2 s)" \
      "grep -q 'ShowSurface(${SURF}, 1)' '${T}/ilm8.log' && grep -q 'the bridge started or restarted' '${T}/shim8.log'" "${T}/shim8.log"
"${PROBE}" --allow-write show 0 > /dev/null
sleep 1
check "Show(0): ShowSurface(${SURF}, 0)" "grep -q 'ShowSurface(${SURF}, 0)' '${T}/ilm8.log'" "${T}/ilm8.log"
kill "${BRIDGE_PID}" 2>/dev/null; wait "${BRIDGE_PID}" 2>/dev/null
"${PROBE}" --allow-write show 1 > /dev/null
sleep 1
check "no bridge running: logged, nothing sent" \
      "N=\$(grep -c 'ShowSurface(${SURF}, 1)' '${T}/ilm8.log'); sleep 1; grep -q 'no video bridge running' '${T}/shim8.log' && [ \$(grep -c 'ShowSurface(${SURF}, 1)' '${T}/ilm8.log') -eq \$N ]" "${T}/shim8.log"
"${T}/aa-bridge" 120 &
BRIDGE_PID=$!
SURF=$((20000000 + BRIDGE_PID))
sleep 3.5
check "a bridge that starts while the screen is up: shown when it appears" "grep -q 'ShowSurface(${SURF}, 1)' '${T}/ilm8.log'" "${T}/ilm8.log"
# The bridge rebuilt its video pipeline in place (same process, same surface id, which comes
# back hidden) and says "video" on the notify socket after the new pipeline's first frame.
N8=$(grep -c "ShowSurface(${SURF}, 1)" "${T}/ilm8.log")
"${FAKE_SYSLOG}" "${AA_SHIM_NOTIFY}" --send video
"${FAKE_SYSLOG}" "${AA_SHIM_NOTIFY}" --send "something a newer bridge says"
sleep 1.5
check "the bridge's 'video' while shown: the surface shown again; an unknown datagram ignored" \
      "[ \$(grep -c 'ShowSurface(${SURF}, 1)' '${T}/ilm8.log') -gt ${N8} ] && grep -q 'video pipeline rebuilt' '${T}/shim8.log' && [ \"\$('${PROBE}' status)\" = 'GetCarLifeConnectStatus() = 3' ]" "${T}/shim8.log"
"${PROBE}" --allow-write show 0 > /dev/null
sleep 1
N8=$(grep -c "ShowSurface(${SURF}, 1)" "${T}/ilm8.log")
"${FAKE_SYSLOG}" "${AA_SHIM_NOTIFY}" --send video
sleep 1.5
check "'video' while the screen is not up: nothing shown (the next Show(1) shows it)" \
      "[ \$(grep -c 'ShowSurface(${SURF}, 1)' '${T}/ilm8.log') -eq ${N8} ]" "${T}/ilm8.log"
kill "${BRIDGE_PID}" 2>/dev/null; wait "${BRIDGE_PID}" 2>/dev/null
stop_shim
kill "${ILM_PID}" "${FAKE_PID}" 2>/dev/null; wait "${ILM_PID}" "${FAKE_PID}" 2>/dev/null

echo "=== 9. the CarLife audio sources in the source service (attach, request, callbacks, detach)"
"${FAKE_SOURCE}" --port 31205 --pause-ms 600 > "${T}/src9.log" 2>&1 &
SRC_PID=$!
CTL9="${T}/ctl9.sock"
start_shim "${T}/shim9.log" AA_SOURCE_PORT=31205 AA_ILM_PORT=0 AA_CTL_SOCK="${CTL9}"
"${FAKE}" --sock "${CTL9}" --step-ms 100 connected projecting "line:mic 1" "line:mic 1" "line:mic 0" \
    "line:audio speech 1" "line:audio speech 0" "line:audio system 1" "line:audio system 0" "line:hostui" \
    wait:3000 "line:audio media 1" > "${T}/fake9.log" 2>&1 &
FAKE_PID=$!
"${PROBE}" listen 5 > "${T}/listen9.log" 2>&1 &
sleep 0.5
"${PROBE}" --allow-write connect > /dev/null
sleep 2
check "session start: registered with the shim's pid, callback connection made" \
      "grep -q 'RegisterClient(${SHIM_PID})' '${T}/src9.log' && grep -q 'callback connection created' '${T}/src9.log'" "${T}/shim9.log"
check "session start: sources 34..37 attached (1), then valid (2)" \
      "[ \"\$(grep -o 'SetSourceStatus([0-9]*, [0-9])' '${T}/src9.log' | tr '\\n' ' ')\" = 'SetSourceStatus(34, 1) SetSourceStatus(35, 1) SetSourceStatus(36, 1) SetSourceStatus(37, 1) SetSourceStatus(34, 2) SetSourceStatus(35, 2) SetSourceStatus(36, 2) SetSourceStatus(37, 2) ' ]" "${T}/src9.log"
check "the phone's back-to-the-car icon publishes OnGoToDeskTop (appconnect leaves the screen)" \
      "grep -q '7 OnGoToDeskTop()' '${T}/listen9.log'" "${T}/listen9.log"
check "nothing asks for the prompt (36) or ASR (37) source" "! grep -qE 'RequestSource\(3[67]' '${T}/src9.log'" "${T}/src9.log"
"${PROBE}" --allow-write show 1 > /dev/null
sleep 1
check "Show(1) alone does not take the speakers (no media source until AA media plays)" \
      "! grep -q 'RequestSource(34' '${T}/src9.log'" "${T}/src9.log"
sleep 3                                       # AA media starts at ~4.5 s
# (fake_receiver prints what it got only after its scripted steps, so this check comes late)
check "back on the AA screen after the car icon: the receiver gets 'home' once" \
      "[ \$(grep -c \"got 'home'\" '${T}/fake9.log') -eq 1 ]" "${T}/fake9.log"
check "the mic, speech and system sounds hold the navigation source (35) once, across the gaps between them" \
      "[ \"\$(grep -oE '(Request|Release)Source\((35|37), [0-9, ]*\)' '${T}/src9.log' | tr '\\n' ' ')\" = 'RequestSource(35, ${SHIM_PID}) ReleaseSource(35, ${SHIM_PID}, 1) ' ]" "${T}/src9.log"
check "the navigation source is given back 2 s after the last use ends" \
      "awk '/receiver: system audio off/ { a = \$2 + 0 } /source: ReleaseSource\\(35/ { b = \$2 + 0 } END { exit !(a > 0 && b - a >= 1.9 && b - a < 3) }' '${T}/shim9.log'" "${T}/shim9.log"
check "AA media plays: RequestSource(34, pid), ConfirmSource(34), PLAY answered 1" \
      "grep -q 'RequestSource(34, ${SHIM_PID})' '${T}/src9.log' && grep -q 'ConfirmSource(34)' '${T}/src9.log' && grep -q 'cbSourceOp(34, 0) -> 1' '${T}/src9.log'" "${T}/shim9.log"
check "another source interrupts: PAUSE is a temporary focus loss for the phone; a PLAY callback is not used" \
      "grep -q 'cbSourceOp(34, 1) -> 1' '${T}/src9.log' && grep -q \"got 'focus transient'\" '${T}/fake9.log' && ! grep -qE \"got 'key (pause|play)'\" '${T}/fake9.log'" "${T}/fake9.log"
"${PROBE}" --allow-write disconnect > /dev/null
sleep 1
check "session end: sources 34..37 detached (6)" "[ \$(grep -c 'SetSourceStatus(3[4-7], 6)' '${T}/src9.log') -eq 4 ]" "${T}/src9.log"
check "fake mode never touches the source service" "! grep -q 'source:' '${T}/shim1.log'"
stop_shim
kill "${SRC_PID}" "${FAKE_PID}" 2>/dev/null; wait "${SRC_PID}" "${FAKE_PID}" 2>/dev/null

echo "=== 9b. a failed attach is tried again until sourcemgr takes it (one lost call meant a silent session)"
"${FAKE_SOURCE}" --port 31215 --no-play --fail-status 2 > "${T}/src9b.log" 2>&1 &
SRC_PID=$!
CTL9B="${T}/ctl9b.sock"
start_shim "${T}/shim9b.log" AA_SOURCE_PORT=31215 AA_SOURCE_LISTEN_PORT=0 AA_ILM_PORT=0 AA_CAN_PORT=0 AA_BT_PORT=0 \
    AA_CTL_SOCK="${CTL9B}"
"${FAKE}" --sock "${CTL9B}" --step-ms 100 connected projecting > "${T}/fake9b.log" 2>&1 &
FAKE_PID=$!
sleep 0.5
"${PROBE}" --allow-write connect > /dev/null
wait_log "${T}/src9b.log" 'SetSourceStatus(37, 2)' 12
check "attach refused twice, then all four attached and valid" \
      "[ \$(grep -c 'refused' '${T}/src9b.log') -eq 2 ] && [ \$(grep -c 'SetSourceStatus(3[4-7], 2)\$' '${T}/src9b.log') -eq 4 ] && [ \$(grep -c 'attaching the sources' '${T}/shim9b.log') -eq 1 ]" "${T}/shim9b.log"
"${PROBE}" --allow-write disconnect > /dev/null
wait_log "${T}/src9b.log" 'SetSourceStatus(37, 6)' 5
check "session end: detached" "[ \$(grep -c 'SetSourceStatus(3[4-7], 6)' '${T}/src9b.log') -eq 4 ]" "${T}/src9b.log"
stop_shim
kill "${SRC_PID}" "${FAKE_PID}" 2>/dev/null; wait "${SRC_PID}" "${FAKE_PID}" 2>/dev/null

echo "=== 10. a sourcemgr that never calls back PLAY (the unit's): getting the media source back gives the focus back"
"${FAKE_SOURCE}" --port 31206 --pause-ms 300 --no-play > "${T}/src11.log" 2>&1 &
SRC_PID=$!
CTL11="${T}/ctl11.sock"
start_shim "${T}/shim11.log" AA_SOURCE_PORT=31206 AA_ILM_PORT=0 AA_CAN_PORT=0 AA_CTL_SOCK="${CTL11}"
"${FAKE}" --sock "${CTL11}" connected projecting "line:audio media 1" > "${T}/fake11.log" 2>&1 &
FAKE_PID=$!
sleep 0.5
"${PROBE}" --allow-write connect > /dev/null  # AA media plays: request -> 1, then PAUSE (the radio after boot)
sleep 2.5
"${PROBE}" --allow-write show 1 > /dev/null    # the user picks the AA screen again: granted
sleep 1.5
check "a PAUSE is a temporary focus loss; the media source granted again (the AA screen opened) gives it back" \
      "[ \"\$(grep -oE \"got 'focus [a-z]+'\" '${T}/fake11.log' | head -2 | tr '\\n' ' ')\" = \"got 'focus transient' got 'focus gain' \" ] && ! grep -q \"got 'key play'\" '${T}/fake11.log'" "${T}/shim11.log"
"${PROBE}" --allow-write disconnect > /dev/null
sleep 0.5
stop_shim
kill "${SRC_PID}" "${FAKE_PID}" 2>/dev/null; wait "${SRC_PID}" "${FAKE_PID}" 2>/dev/null

echo "=== 11. media paused for our own mic source (the assistant): left to Android Auto"
"${FAKE_SOURCE}" --port 31207 --pause-ms 300 --no-play > "${T}/src12.log" 2>&1 &
SRC_PID=$!
CTL12="${T}/ctl12.sock"
start_shim "${T}/shim12.log" AA_SOURCE_PORT=31207 AA_ILM_PORT=0 AA_CAN_PORT=0 AA_CTL_SOCK="${CTL12}"
"${FAKE}" --sock "${CTL12}" --step-ms 100 connected projecting "line:mic 1" "line:audio media 1" > "${T}/fake12.log" 2>&1 &
FAKE_PID=$!
sleep 0.5
"${PROBE}" --allow-write connect > /dev/null  # request 34 -> 1, then PAUSE while 35 is held
sleep 2
check "no 'key pause' while our navigation source (the assistant) holds the audio" \
      "grep -q 'media paused while our navigation source is held' '${T}/shim12.log' && ! grep -q \"got 'key pause'\" '${T}/fake12.log'" "${T}/shim12.log"
"${PROBE}" --allow-write disconnect > /dev/null
sleep 0.5
stop_shim
kill "${SRC_PID}" "${FAKE_PID}" 2>/dev/null; wait "${SRC_PID}" "${FAKE_PID}" 2>/dev/null

echo "=== 12. vehicle data from the CAN service: day/night, gear, driver's side (GetCanCarplayInfo), speed (publisher)"
"${FAKE_CAN}" --port 31705 --night-after-ms 3000 --pub-port 31706 --speed-raw 3600 > "${T}/can10.log" 2>&1 &
CAN_PID=$!
CTL10="${T}/ctl10.sock"
start_shim "${T}/shim10.log" AA_CAN_PORT=31705 AA_CAN_LISTEN_PORT=31706 AA_SOURCE_PORT=0 AA_ILM_PORT=0 AA_CTL_SOCK="${CTL10}"
"${FAKE}" --sock "${CTL10}" > "${T}/fake10.log" 2>&1 &
FAKE_PID=$!
sleep 7
check "day, then night: the receiver gets 'night 0', then 'night 1' once DayNight turns 1" \
      "[ \"\$(grep -oE \"got 'night [01]'\" '${T}/fake10.log' | uniq | tr '\\n' ' ')\" = \"got 'night 0' got 'night 1' \" ]" "${T}/fake10.log"
check "each change logged once, with the record" "[ \$(grep -c 'can: DayNight' '${T}/shim10.log') -eq 2 ] && grep -q 'can: DayNight 1 (transmission 1, right-hand drive 0) -> night' '${T}/shim10.log'" "${T}/shim10.log"
check "fake mode never asks the CAN service" "! grep -q 'can:' '${T}/shim1.log'"
check "gear (transmission 1) and driver's side reach the receiver" \
      "grep -q \"got 'gear park'\" '${T}/fake10.log' && grep -q \"got 'rhd 0'\" '${T}/fake10.log'" "${T}/fake10.log"
check "the speed comes from the CAN publisher: 36 km/h -> 'speed 10.000'" \
      "grep -q 'subscribed to the CAN service' '${T}/shim10.log' && grep -q 'GetCanVehicleSpeed' '${T}/can10.log' && grep -q \"got 'speed 10.000'\" '${T}/fake10.log'" "${T}/shim10.log"
check "an old receiver (no hello) is never pinged" "! grep -q \"got 'ping\" '${T}/fake10.log'" "${T}/fake10.log"
kill "${FAKE_PID}" 2>/dev/null; wait "${FAKE_PID}" 2>/dev/null
"${FAKE}" --sock "${CTL10}" > "${T}/fake10b.log" 2>&1 &
FAKE_PID=$!
sleep 2.5
check "a receiver that connects later (restarted) has night and the driver's side within one poll" \
      "grep -q \"got 'night 1'\" '${T}/fake10b.log' && grep -q \"got 'rhd 0'\" '${T}/fake10b.log'" "${T}/fake10b.log"
stop_shim
kill "${CAN_PID}" "${FAKE_PID}" 2>/dev/null; wait "${CAN_PID}" "${FAKE_PID}" 2>/dev/null

echo "=== 13. the phone's mic: CarLife voice module (4) status for appconnect, and the BT mic path"
"${FAKE_BT}" --port 31315 > "${T}/bt13.log" 2>&1 &
BT_PID=$!
CTL13="${T}/ctl13.sock"
start_shim "${T}/shim13.log" AA_BT_PORT=31315 AA_SOURCE_PORT=0 AA_ILM_PORT=0 AA_CAN_PORT=0 AA_CTL_SOCK="${CTL13}"
"${FAKE}" --sock "${CTL13}" --step-ms 1500 connected projecting "line:mic 1" "line:mic 0" > "${T}/fake13.log" 2>&1 &
FAKE_PID=$!
"${PROBE}" listen 9 > "${T}/listen13.log" 2>&1 &
sleep 0.5
"${PROBE}" --allow-write connect > /dev/null
sleep 4.8                                   # mic 1 came at ~4.5 s
"${PROBE}" ro > "${T}/ro13on.log" 2>&1
sleep 1.7                                   # mic 0 came at ~6 s
"${PROBE}" ro > "${T}/ro13off.log" 2>&1
sleep 2.5
check "mic on: OnModuleStatusChange(4, 1), GetModuleStatus(4) = 1, BT path 1 (the mic to the SoC)" \
      "grep -q '8 OnModuleStatusChange(4, 1)' '${T}/listen13.log' && grep -q 'GetModuleStatus(4) = 1' '${T}/ro13on.log' && grep -q 'SetECNRAudioPath(1)' '${T}/bt13.log'" "${T}/shim13.log"
check "mic off: OnModuleStatusChange(4, 0), GetModuleStatus(4) = 0, BT path 0, in that order" \
      "grep -q '8 OnModuleStatusChange(4, 0)' '${T}/listen13.log' && grep -q 'GetModuleStatus(4) = 0' '${T}/ro13off.log' && [ \"\$(tr '\\n' ' ' < '${T}/bt13.log' | grep -o 'SetECNRAudioPath([01])' | tr '\\n' ' ')\" = 'SetECNRAudioPath(0) SetECNRAudioPath(1) SetECNRAudioPath(0) ' ]" "${T}/bt13.log"
check "start-up: the mic path put back to Bluetooth (0) first (a shim that died mid-assistant left it)" \
      "[ \"\$(grep -o 'SetECNRAudioPath([01])' '${T}/bt13.log' | head -1)\" = 'SetECNRAudioPath(0)' ]" "${T}/bt13.log"
check "fake mode never touches the BT service" "! grep -q 'bt:' '${T}/shim1.log'"
check "no btmac line while the BT service reports 00:00:00:00:00:00" \
      "grep -q 'GetAttribute(3)' '${T}/bt13.log' && ! grep -q 'btmac' '${T}/fake13.log'" "${T}/fake13.log"
"${PROBE}" --allow-write disconnect > /dev/null
sleep 0.5
stop_shim
kill "${BT_PID}" "${FAKE_PID}" 2>/dev/null; wait "${BT_PID}" "${FAKE_PID}" 2>/dev/null

echo "=== 14. the unit's Bluetooth address (GetAttribute 3) -> btmac to each receiver connection"
"${FAKE_BT}" --port 31316 --addr 02:00:00:bb:00:01 > "${T}/bt14.log" 2>&1 &
BT_PID=$!
CTL14="${T}/ctl14.sock"
start_shim "${T}/shim14.log" AA_BT_PORT=31316 AA_SOURCE_PORT=0 AA_ILM_PORT=0 AA_CAN_PORT=0 AA_CTL_SOCK="${CTL14}"
sleep 1.5                                   # the address is known before any receiver connects
"${FAKE}" --sock "${CTL14}" > "${T}/fake14a.log" 2>&1 &
FAKE_PID=$!
sleep 2
kill "${FAKE_PID}" 2>/dev/null; wait "${FAKE_PID}" 2>/dev/null
"${FAKE}" --sock "${CTL14}" > "${T}/fake14b.log" 2>&1 &
FAKE_PID=$!
sleep 2
check "a receiver that connects later gets 'btmac 02:00:00:BB:00:01' once" \
      "[ \$(grep -c \"got 'btmac 02:00:00:BB:00:01'\" '${T}/fake14a.log') -eq 1 ]" "${T}/fake14a.log"
check "a reconnected receiver gets it again" "grep -q \"got 'btmac 02:00:00:BB:00:01'\" '${T}/fake14b.log'" "${T}/fake14b.log"
check "the address is asked for once and logged" \
      "[ \$(grep -c 'GetAttribute(3)' '${T}/bt14.log') -eq 1 ] && grep -q \"bt: the unit's Bluetooth address is 02:00:00:BB:00:01\" '${T}/shim14.log'" "${T}/shim14.log"
stop_shim
kill "${BT_PID}" "${FAKE_PID}" 2>/dev/null; wait "${BT_PID}" "${FAKE_PID}" 2>/dev/null

echo "=== 15. audio focus from sourcemgr's status topic: a call (restored), the radio (for good); the car's mute"
"${FAKE_SOURCE}" --port 31208 --pause-ms 400 --no-play --pub-port 31209 --taker 7 --restore > "${T}/src15.log" 2>&1 &
SRC_PID=$!
CTL15="${T}/ctl15.sock"
start_shim "${T}/shim15.log" AA_SOURCE_PORT=31208 AA_SOURCE_LISTEN_PORT=31209 AA_ILM_PORT=0 AA_CAN_PORT=0 \
    AA_BT_PORT=0 AA_CTL_SOCK="${CTL15}"
"${FAKE}" --sock "${CTL15}" connected projecting "line:audio media 1" > "${T}/fake15.log" 2>&1 &
FAKE_PID=$!
sleep 0.5
"${PROBE}" --allow-write connect > /dev/null  # media at ~0.9 s: request, PAUSE, TEL active, then 34 restored
sleep 3
check "a call (TEL 7) interrupts: temporary loss; (34, SS_RESTORED): the source taken back and the focus with it" \
      "[ \"\$(grep -oE \"got 'focus [a-z]+'\" '${T}/fake15.log' | tr '\\n' ' ')\" = \"got 'focus transient' got 'focus gain' \" ] && [ \$(grep -c 'RequestSource(34' '${T}/src15.log') -eq 2 ] && grep -q 'subscribed to the source status topic' '${T}/shim15.log'" "${T}/shim15.log"
for c in 0 0 1 1; do "${PROBE}" --allow-write modcmd 3 "${c}" > /dev/null; done
sleep 0.5
check "the car's mute (module 3: 0, 0, 1, 1): one temporary loss, one gain" \
      "[ \"\$(grep -oE \"got 'focus [a-z]+'\" '${T}/fake15.log' | tr '\\n' ' ')\" = \"got 'focus transient' got 'focus gain' got 'focus transient' got 'focus gain' \" ]" "${T}/fake15.log"
"${PROBE}" --allow-write modcmd 3 1 > /dev/null
sleep 0.3
check "an unmute the shim did not pause for (session start, call end) is ignored" \
      "[ \$(grep -c \"got 'focus gain'\" '${T}/fake15.log') -eq 2 ]" "${T}/fake15.log"
"${PROBE}" --allow-write disconnect > /dev/null
sleep 0.5
stop_shim
kill "${SRC_PID}" "${FAKE_PID}" 2>/dev/null; wait "${SRC_PID}" "${FAKE_PID}" 2>/dev/null

"${FAKE_SOURCE}" --port 31210 --pause-ms 400 --no-play --pub-port 31211 --taker 1 > "${T}/src15b.log" 2>&1 &
SRC_PID=$!
CTL15B="${T}/ctl15b.sock"
start_shim "${T}/shim15b.log" AA_SOURCE_PORT=31210 AA_SOURCE_LISTEN_PORT=31211 AA_ILM_PORT=0 AA_CAN_PORT=0 \
    AA_BT_PORT=0 AA_CTL_SOCK="${CTL15B}"
"${FAKE}" --sock "${CTL15B}" connected projecting "line:audio media 1" wait:1500 "line:audio media 0" \
    wait:2500 "line:audio media 1" > "${T}/fake15b.log" 2>&1 &
FAKE_PID=$!
sleep 0.5
"${PROBE}" --allow-write connect > /dev/null  # media at ~0.9 s: request, PAUSE, TUNER active; stops at ~2.7 s
sleep 3.5
"${PROBE}" --allow-write show 1 > /dev/null    # the APP key: back on the AA screen, AA silent
sleep 1
check "the radio (TUNER 1) took over; back on the AA screen with AA silent: the radio keeps the speakers" \
      "[ \$(grep -c 'RequestSource(34' '${T}/src15b.log') -eq 1 ] && grep -q 'Show(1)' '${T}/shim15b.log' && ! grep -q 'focus: gain' '${T}/shim15b.log'" "${T}/shim15b.log"
sleep 2.5                                     # AA media plays again at ~5.5 s (the user pressed play)
check "AA media plays again: the media source taken back, the focus with it, no extra play key" \
      "[ \$(grep -c 'RequestSource(34' '${T}/src15b.log') -eq 2 ] && [ \"\$(grep -oE \"got '(focus [a-z]+|key (pause|play))'\" '${T}/fake15b.log' | tr '\\n' ' ')\" = \"got 'focus transient' got 'focus loss' got 'key pause' got 'focus gain' \" ]" "${T}/fake15b.log"
"${PROBE}" --allow-write disconnect > /dev/null
sleep 0.5
stop_shim
kill "${SRC_PID}" "${FAKE_PID}" 2>/dev/null; wait "${SRC_PID}" "${FAKE_PID}" 2>/dev/null

echo "=== 16. Android Auto's Bluetooth pairing with the car (btpair -> btpaired, btauth, AcceptSspRequest)"
"${FAKE_BT}" --port 31317 --pub-port 31318 --noise --paired 11:22:33:44:55:66 --phone 02:00:00:AA:00:02 \
    --confirm-after-ms 500 --stranger 22:33:44:55:66:77 > "${T}/bt16.log" 2>&1 &
BT_PID=$!
CTL16="${T}/ctl16.sock"
start_shim "${T}/shim16.log" AA_BT_PORT=31317 AA_BT_LISTEN_PORT=31318 AA_BT_PAIRING=1 AA_SOURCE_PORT=0 \
    AA_ILM_PORT=0 AA_CAN_PORT=0 AA_CTL_SOCK="${CTL16}"
sleep 1.5                                   # subscribed to the events
"${FAKE}" --sock "${CTL16}" connected projecting "line:btpair 11:22:33:44:55:66 2" wait:1000 \
    "line:btpair 02:00:00:AA:00:02 2" > "${T}/fake16.log" 2>&1 &
FAKE_PID=$!
sleep 0.5
"${PROBE}" --allow-write connect > /dev/null  # the shim sends "start": the receiver's script runs
sleep 4
check "events the loop does not use (btservice sends many) are taken, not fatal" \
      "! grep -q 'Assertion' '${T}/shim16.log' && kill -0 \${SHIM_PID}" "${T}/shim16.log"
check "a phone in the car's paired list: 'btpaired 1'; an unknown one: 'btpaired 0'" \
      "[ \"\$(grep -oE \"got 'btpaired [01]'\" '${T}/fake16.log' | tr '\\n' ' ')\" = \"got 'btpaired 1' got 'btpaired 0' \" ] && grep -q 'subscribed to the Bluetooth events' '${T}/shim16.log'" "${T}/shim16.log"
check "unknown phone: auto-accept reset at start, then on for it, its key to the phone, AcceptSspRequest(1), put back" \
      "grep -q \"got 'btauth 123456'\" '${T}/fake16.log' && [ \"\$(grep -oE '(SetAutoAcceptConnect|AcceptSspRequest)\\([01]\\)' '${T}/bt16.log' | tr '\\n' ' ')\" = 'SetAutoAcceptConnect(0) SetAutoAcceptConnect(1) AcceptSspRequest(1) SetAutoAcceptConnect(0) ' ] && grep -q 'pairing with 02:00:00:AA:00:02 done' '${T}/shim16.log'" "${T}/shim16.log"
check "inside the window another device's pairing request is left to the Bluetooth app, its key not sent" \
      "grep -q 'PairingComfirmRequest(22:33:44:55:66:77) (not ours' '${T}/shim16.log' && ! grep -q \"got 'btauth 654321'\" '${T}/fake16.log'" "${T}/shim16.log"
"${PROBE}" --allow-write disconnect > /dev/null
sleep 0.5
stop_shim
kill "${FAKE_PID}" 2>/dev/null; wait "${FAKE_PID}" 2>/dev/null
CTL16B="${T}/ctl16b.sock"
start_shim "${T}/shim16b.log" AA_BT_PORT=31317 AA_BT_LISTEN_PORT=31318 AA_BT_PAIRING=0 AA_SOURCE_PORT=0 AA_ILM_PORT=0 \
    AA_CAN_PORT=0 AA_CTL_SOCK="${CTL16B}"
"${FAKE}" --sock "${CTL16B}" connected projecting "line:btpair 00:11:22:33:44:55 2" > "${T}/fake16b.log" 2>&1 &
FAKE_PID=$!
sleep 0.5
"${PROBE}" --allow-write connect > /dev/null
sleep 1.5
check "pairing loop off (AA_BT_PAIRING=0): 'btpaired 1' at once, the paired list not asked" \
      "grep -q \"got 'btpaired 1'\" '${T}/fake16b.log' && [ \$(grep -c 'GetDeviceNum' '${T}/bt16.log') -eq 2 ]" "${T}/shim16b.log"
"${PROBE}" --allow-write disconnect > /dev/null
sleep 0.5
stop_shim
kill "${BT_PID}" "${FAKE_PID}" 2>/dev/null; wait "${BT_PID}" "${FAKE_PID}" 2>/dev/null

# The phone forgot the car, which still lists it: "paired", then the phone bonds anyway; the
# shim takes that confirm request too (step 3 run 38: left to the Bluetooth app, it failed).
"${FAKE_BT}" --port 31321 --pub-port 31322 --paired 02:00:00:AA:00:01 --phone 02:00:00:AA:00:01 \
    --confirm-after-ms 800 --rebond > "${T}/bt16d.log" 2>&1 &
BT_PID=$!
CTL16D="${T}/ctl16d.sock"
start_shim "${T}/shim16d.log" AA_BT_PORT=31321 AA_BT_LISTEN_PORT=31322 AA_SOURCE_PORT=0 AA_ILM_PORT=0 \
    AA_CAN_PORT=0 AA_CTL_SOCK="${CTL16D}"
sleep 1.5
"${FAKE}" --sock "${CTL16D}" connected projecting "line:btpair 02:00:00:AA:00:01 2" > "${T}/fake16d.log" 2>&1 &
FAKE_PID=$!
sleep 0.5
"${PROBE}" --allow-write connect > /dev/null
sleep 3
check "the phone forgot the car: 'btpaired 1', then its re-bond confirmed (key to the phone, AcceptSspRequest(1))" \
      "grep -q \"got 'btpaired 1'\" '${T}/fake16d.log' && grep -q \"got 'btauth 123456'\" '${T}/fake16d.log' && grep -q 'AcceptSspRequest(1)' '${T}/bt16d.log' && grep -q 'paired again (done)' '${T}/shim16d.log'" "${T}/shim16d.log"
"${PROBE}" --allow-write disconnect > /dev/null
sleep 0.5
stop_shim
kill "${BT_PID}" "${FAKE_PID}" 2>/dev/null; wait "${BT_PID}" "${FAKE_PID}" 2>/dev/null

# PIN pairing (the phone asked with method 4): the PIN goes to the phone first, the car's PIN
# request is answered with it.
"${FAKE_BT}" --port 31319 --pub-port 31320 --phone 02:00:00:AA:00:01 --confirm-after-ms 500 --pin \
    > "${T}/bt16c.log" 2>&1 &
BT_PID=$!
CTL16C="${T}/ctl16c.sock"
start_shim "${T}/shim16c.log" AA_BT_PORT=31319 AA_BT_LISTEN_PORT=31320 AA_SOURCE_PORT=0 AA_ILM_PORT=0 \
    AA_CAN_PORT=0 AA_CTL_SOCK="${CTL16C}"
sleep 1.5
"${FAKE}" --sock "${CTL16C}" connected projecting "line:btpair 02:00:00:AA:00:01 4" > "${T}/fake16c.log" 2>&1 &
FAKE_PID=$!
sleep 0.5
"${PROBE}" --allow-write connect > /dev/null
sleep 3
PIN16="$(grep -oE "got 'btauth [0-9]{6}'" "${T}/fake16c.log" | grep -oE '[0-9]{6}' | head -1)"
check "PIN pairing: 'btpaired 0', a 6-digit PIN to the phone, the car's PIN request answered with it" \
      "grep -q \"got 'btpaired 0'\" '${T}/fake16c.log' && [ -n '${PIN16}' ] && grep -q 'AcceptPinCodeRequest(${PIN16})' '${T}/bt16c.log' && grep -q 'pairing with 02:00:00:AA:00:01 done' '${T}/shim16c.log'" "${T}/shim16c.log"
"${PROBE}" --allow-write disconnect > /dev/null
sleep 0.5
stop_shim
kill "${BT_PID}" "${FAKE_PID}" 2>/dev/null; wait "${BT_PID}" "${FAKE_PID}" 2>/dev/null

echo "=== 17. the phone's Bluetooth music (A2DP) off during the session, back after; the call pop-up"
"${FAKE_BT}" --port 31323 --pub-port 31324 --a2dp 02:00:00:aa:00:01 --a2dp-back-ms 700 > "${T}/bt17.log" 2>&1 &
BT_PID=$!
CTL17="${T}/ctl17.sock"
start_shim "${T}/shim17.log" AA_BT_PORT=31323 AA_BT_LISTEN_PORT=31324 AA_SOURCE_PORT=0 AA_ILM_PORT=0 \
    AA_CAN_PORT=0 AA_CALL_FIX_MS=5000 AA_CTL_SOCK="${CTL17}"
sleep 1.5                                   # subscribed to the events
"${FAKE}" --sock "${CTL17}" > "${T}/fake17.log" 2>&1 &
FAKE_PID=$!
sleep 0.5
"${PROBE}" --allow-write connect > /dev/null  # projecting at ~0.6 s: A2DP off; back at +0.7 s: off again
"${PROBE}" --allow-write show 1 > /dev/null
sleep 2
"${PROBE}" ro > "${T}/ro17on.log" 2>&1
check "session: the device holding A2DP read, A2DP dropped, dropped again when it came back" \
      "[ \$(grep -c 'Disconnect(2)' '${T}/bt17.log') -eq 2 ] && grep -q 'connected during the session: off again' '${T}/shim17.log'" "${T}/shim17.log"
check "AA on screen: NeedBlockBTPopup 1 (no call pop-up over AA)" "grep -q 'NeedBlockBTPopup() = 1' '${T}/ro17on.log'" "${T}/ro17on.log"
# No appconnect process here, so its in-memory patch (appc_patch.hpp; tools/appc_patch_test.cpp
# tests the patch itself natively) is reported missing at connect and Show(1), and the timing fix
# stands in: NeedBlockBTPopup with no call running means an outgoing call may be starting, so
# appconnect's next status question gets 2 once (it then skips launching the phone app), and 3 after.
check "appconnect patch: no appconnect process reported, nothing patched" \
      "grep -q 'appconnect patch: no appconnect process' '${T}/shim17.log'" "${T}/shim17.log"
S1="$("${PROBE}" status)"; S2="$("${PROBE}" status)"
check "call fix: the status question after it answers 2 once, then 3 again" \
      "[ '${S1}' = 'GetCarLifeConnectStatus() = 2' ] && [ '${S2}' = 'GetCarLifeConnectStatus() = 3' ]" "${T}/shim17.log"
"${PROBE}" --allow-write disconnect > /dev/null
sleep 3
check "session over: A2DP back to that device 2 s later (Connect(addr, 2))" \
      "grep -q 'Connect(02:00:00:aa:00:01, 2)' '${T}/bt17.log'" "${T}/bt17.log"
"${PROBE}" ro > "${T}/ro17off.log" 2>&1
check "no session: NeedBlockBTPopup 0" "grep -q 'NeedBlockBTPopup() = 0' '${T}/ro17off.log'" "${T}/ro17off.log"
stop_shim
kill "${BT_PID}" "${FAKE_PID}" 2>/dev/null; wait "${BT_PID}" "${FAKE_PID}" 2>/dev/null

echo "=== 18. the unit switched off and on: AA's media pauses and resumes, its video too; last mode"
LM18="${T}/lastmode18"
CMD18="${T}/power18.cmd"
BOOTS18="${T}/boots18"
printf 'speakers car\n' > "${LM18}"
printf '2\n' > "${BOOTS18}"
: > "${CMD18}"
"${FAKE_SOURCE}" --port 31330 --no-play --pub-port 31331 > "${T}/src18.log" 2>&1 &
SRC_PID=$!
"${FAKE_POWER}" --port 31332 --pub-port 31333 --cmd "${CMD18}" > "${T}/power18.log" 2>&1 &
PWR_PID=$!
CTL18="${T}/ctl18.sock"
start_shim "${T}/shim18.log" AA_SOURCE_PORT=31330 AA_SOURCE_LISTEN_PORT=31331 AA_POWER_PORT=31332 \
    AA_POWER_LISTEN_PORT=31333 AA_LAST_MODE_FILE="${LM18}" AA_ILM_PORT=0 AA_CAN_PORT=0 AA_BT_PORT=0 \
    AA_CTL_SOCK="${CTL18}" AA_BOOTS_FILE="${BOOTS18}"
"${FAKE}" --sock "${CTL18}" connected projecting "line:audio media 1" > "${T}/fake18.log" 2>&1 &
FAKE_PID=$!
wait_log "${T}/power18.log" 'GetPowerState -> 3' 10
check "power: subscribed, the current state read" \
      "grep -q 'power: subscribed to the power events' '${T}/shim18.log' && grep -q 'GetPowerState -> 3' '${T}/power18.log'" "${T}/shim18.log"
"${PROBE}" --allow-write connect > /dev/null   # last mode: the car had the speakers
"${PROBE}" --allow-write show 1 > /dev/null
sleep 2
check "last mode 'car': the receiver hears 'autoplay 0' before 'start'" \
      "[ \"\$(grep -oE \"got '(autoplay [01]|start)'\" '${T}/fake18.log' | tr '\\n' ' ')\" = \"got 'autoplay 0' got 'start' \" ]" "${T}/fake18.log"
echo "4 2 0" >> "${CMD18}"                      # PS_USER_OFF, can be woken
wait_log "${T}/fake18.log" "got 'show 0'" 10
echo "3 0 0" >> "${CMD18}"                      # PS_USER_ON
wait_log "${T}/fake18.log" "got 'show 1'" 10 2
check "switched off: a temporary focus loss and 'show 0'; on: the gain and 'show 1'" \
      "[ \"\$(grep -oE \"got '(focus [a-z]+|show [01])'\" '${T}/fake18.log' | tr '\\n' ' ')\" = \"got 'show 1' got 'focus transient' got 'show 0' got 'focus gain' got 'show 1' \" ]" "${T}/fake18.log"
check "a switch-off clears the launcher's boot-loop counter" \
      "[ \"\$(cat '${BOOTS18}')\" = 0 ] && grep -q 'boot-loop counter cleared' '${T}/shim18.log'" "${T}/shim18.log"
check "AA had the speakers at the switch-off (granted 2 s before): last mode written 'speakers aa'" \
      "[ \"\$(cat '${LM18}')\" = 'speakers aa' ] && grep -q 'last mode: Android Auto now has the speakers' '${T}/shim18.log'" "${T}/shim18.log"
"${PROBE}" --allow-write disconnect > /dev/null
sleep 0.5
stop_shim
kill "${SRC_PID}" "${PWR_PID}" "${FAKE_PID}" 2>/dev/null; wait "${SRC_PID}" "${PWR_PID}" "${FAKE_PID}" 2>/dev/null

echo "=== 19. the receiver link: hello both ways; a protocol 2 receiver is pinged, and killed when it hangs"
CTL19="${T}/ctl19.sock"
start_shim "${T}/shim19.log" AA_CTL_SOCK="${CTL19}" AA_ILM_PORT=0 AA_SOURCE_PORT=0 AA_CAN_PORT=0 AA_BT_PORT=0 \
    AA_AUDIO_PORT=0
# Answers the first ping only, then plays hung; named as the real receiver.
"${FAKE}" --sock "${CTL19}" --hello --pongs 1 --comm aa-headless --step-ms 100 connected projecting \
    > "${T}/fake19.log" 2>&1 &
FAKE_PID=$!
"${PROBE}" listen 16 > "${T}/listen19.log" 2>&1 &
LISTEN_PID=$!
sleep 1
"${PROBE}" --allow-write connect > /dev/null
wait "${FAKE_PID}"
RC19=$?
wait "${LISTEN_PID}"
check "the shim says hello 2 first, and pings a receiver that said hello 2" \
      "[ \"\$(grep -o \"got '[a-z]*\" '${T}/fake19.log' | head -1)\" = \"got 'hello\" ] && grep -q \"got 'hello 2'\" '${T}/fake19.log' && grep -q \"got 'ping 1'\" '${T}/fake19.log' && grep -q 'receiver: protocol 2' '${T}/shim19.log'" "${T}/shim19.log"
check "3 pings unanswered: the receiver is killed (SIGKILL, procguard restarts it) and the session ends: 2 3 4" \
      "[ ${RC19} -eq 137 ] && grep -q 'no answer to 3 pings' '${T}/shim19.log' && grep -q 'killed; procguard restarts it' '${T}/shim19.log' && [ \"\$(statuses '${T}/listen19.log')\" = '2 3 4' ]" "${T}/shim19.log"
# Another process name: the connection is dropped, the process left alone.
"${FAKE}" --sock "${CTL19}" --hello --pongs 0 --comm not-the-receiv > "${T}/fake19b.log" 2>&1 &
FAKE_PID=$!
wait "${FAKE_PID}"
RC19=$?
check "a hung peer that is not the receiver: not killed, its connection dropped" \
      "[ ${RC19} -eq 0 ] && grep -q 'shim closed the connection' '${T}/fake19b.log' && grep -q 'not-the-receiv\", not aa-headless,aa-receiver; not killed' '${T}/shim19.log'" "${T}/shim19.log"
stop_shim

echo "=== 20. a second receiver connection replaces the first: the first one's session is over (4)"
CTL20="${T}/ctl20.sock"
start_shim "${T}/shim20.log" AA_CTL_SOCK="${CTL20}" AA_ILM_PORT=0 AA_SOURCE_PORT=0 AA_CAN_PORT=0 AA_BT_PORT=0 \
    AA_AUDIO_PORT=0
"${FAKE}" --sock "${CTL20}" --step-ms 100 connected projecting > "${T}/fake20a.log" 2>&1 &
FAKE_PID=$!
"${PROBE}" listen 5 > "${T}/listen20.log" 2>&1 &
LISTEN_PID=$!
sleep 1
"${PROBE}" --allow-write connect > /dev/null
sleep 1
"${FAKE}" --sock "${CTL20}" > "${T}/fake20b.log" 2>&1 &
FAKE2_PID=$!
wait "${LISTEN_PID}"
check "replaced: 'state gone' for the old connection, the listener saw 2 3 4" \
      "grep -q 'replacing the previous connection' '${T}/shim20.log' && [ \"\$(statuses '${T}/listen20.log')\" = '2 3 4' ]" "${T}/shim20.log"
stop_shim
kill "${FAKE_PID}" "${FAKE2_PID}" 2>/dev/null; wait "${FAKE_PID}" "${FAKE2_PID}" 2>/dev/null

echo "=== 21. SIGTERM during a session: (4, 0) published, sources detached, the mic back, then exit 0"
"${FAKE_SOURCE}" --port 31240 --no-play > "${T}/src21.log" 2>&1 &
SRC_PID=$!
"${FAKE_BT}" --port 31340 > "${T}/bt21.log" 2>&1 &
BT_PID=$!
CTL21="${T}/ctl21.sock"
start_shim "${T}/shim21.log" AA_CTL_SOCK="${CTL21}" AA_SOURCE_PORT=31240 AA_SOURCE_LISTEN_PORT=0 AA_BT_PORT=31340 \
    AA_BT_LISTEN_PORT=0 AA_ILM_PORT=0 AA_CAN_PORT=0 AA_AUDIO_PORT=0
"${FAKE}" --sock "${CTL21}" --step-ms 100 connected projecting "line:mic 1" > "${T}/fake21.log" 2>&1 &
FAKE_PID=$!
"${PROBE}" listen 12 > "${T}/listen21.log" 2>&1 &
LISTEN_PID=$!
sleep 1
"${PROBE}" --allow-write connect > /dev/null
wait_log "${T}/bt21.log" 'SetECNRAudioPath(1)' 5
wait_log "${T}/src21.log" 'SetSourceStatus(37, 2)' 5
T0=$(date +%s%N)
stop_shim
T1=$(date +%s%N)
wait "${LISTEN_PID}"
check "exit 0 within 5 s" "[ ${SHIM_RC} -eq 0 ] && [ \$(( (T1 - T0) / 1000000 )) -lt 5000 ]" "${T}/shim21.log"
check "the listener heard the session end (4) before the shim went" "[ \"\$(statuses '${T}/listen21.log')\" = '2 3 4' ]" "${T}/listen21.log"
check "the sources detached and the mic path back to Bluetooth (0)" \
      "[ \$(grep -c 'SetSourceStatus(3[4-7], 6)' '${T}/src21.log') -eq 4 ] && [ \"\$(grep -o 'SetECNRAudioPath([01])' '${T}/bt21.log' | tail -1)\" = 'SetECNRAudioPath(0)' ]" "${T}/shim21.log"
check "the receiver was told to stop" "grep -q \"got 'stop'\" '${T}/fake21.log'" "${T}/fake21.log"
kill "${SRC_PID}" "${BT_PID}" "${FAKE_PID}" 2>/dev/null; wait "${SRC_PID}" "${BT_PID}" "${FAKE_PID}" 2>/dev/null

echo "=== 22. logging: syslog for every mode but console, never blocking; the log relay"
"${FAKE_SYSLOG}" "${T}/log.sock" > "${T}/syslog22.log" 2>&1 &
SYSLOG_PID=$!
sleep 0.5
AA_SHIM_SYSLOG_SOCK="${T}/log.sock" AA_SHIM_FAKE=1 "${SHIM}" --debug-output none > "${T}/shim22.log" 2>&1 &
SHIM_PID=$!
wait_for_file "${REG}"
sleep 0.5
check "--debug-output none: to syslog (user.info, ident, pid, monotonic stamp), nothing on stderr" \
      "grep -qE '^<14>aa-carlife-shim\\[${SHIM_PID}\\]: \\[[0-9]+\\.[0-9]{3}\\] INFO: ready' '${T}/syslog22.log' && [ ! -s '${T}/shim22.log' ]" "${T}/syslog22.log"
stop_shim
check "  ... and exit 0" "[ ${SHIM_RC} -eq 0 ]"
printf 'one\ntwo\r\n\nthree' | AA_SHIM_SYSLOG_SOCK="${T}/log.sock" "${SHIM}" --log-relay aa-test
RC22=$?
sleep 0.5
check "log relay: each line one message with the tag, exit 0 at the end of input" \
      "[ ${RC22} -eq 0 ] && grep -qE '^<14>aa-test: \\[[0-9.]+\\] INFO: one\$' '${T}/syslog22.log' && grep -qE 'INFO: two\$' '${T}/syslog22.log' && grep -qE 'INFO: three\$' '${T}/syslog22.log' && [ \$(grep -c '^<14>aa-test:' '${T}/syslog22.log') -eq 3 ]" "${T}/syslog22.log"
kill "${SYSLOG_PID}" 2>/dev/null; wait "${SYSLOG_PID}" 2>/dev/null
# A syslog that reads nothing for 3 s (its queue holds 10 datagrams here): lines are dropped,
# never waited for, and the count comes once lines get through again.
"${FAKE_SYSLOG}" "${T}/slow.sock" --stall-ms 3000 > "${T}/slow22.log" 2>&1 &
SYSLOG_PID=$!
sleep 0.5
T0=$(date +%s%N)
{ seq 1 3000; sleep 4; echo after; } | AA_SHIM_SYSLOG_SOCK="${T}/slow.sock" "${SHIM}" --log-relay aa-test
RC22=$?
T1=$(date +%s%N)
sleep 0.5
check "a stalled syslog: the relay never blocks, drops, then reports how many" \
      "[ ${RC22} -eq 0 ] && [ \$(( (T1 - T0) / 1000000 )) -lt 15000 ] && grep -q 'INFO: after' '${T}/slow22.log' && grep -qE 'WARN: [0-9]+ log lines dropped' '${T}/slow22.log'" "${T}/slow22.log"
kill "${SYSLOG_PID}" 2>/dev/null; wait "${SYSLOG_PID}" 2>/dev/null
"${FAKE_SYSLOG}" "${T}/stall.sock" --stall > "${T}/stall22.log" 2>&1 &
SYSLOG_PID=$!
sleep 0.5
AA_SHIM_SYSLOG_SOCK="${T}/stall.sock" AA_SHIM_FAKE=1 AA_SHIM_VERBOSE=1 "${SHIM}" --debug-output none > "${T}/shim22b.log" 2>&1 &
SHIM_PID=$!
wait_for_file "${REG}"
for _ in $(seq 1 20); do "${PROBE}" status > /dev/null; done
check "syslog never reading: the shim still answers" "[ \"\$('${PROBE}' status)\" = 'GetCarLifeConnectStatus() = 0' ]"
stop_shim
check "  ... and exits 0" "[ ${SHIM_RC} -eq 0 ]"
kill "${SYSLOG_PID}" 2>/dev/null; wait "${SYSLOG_PID}" 2>/dev/null
AA_SHIM_SYSLOG_SOCK="${T}/none.sock" AA_SHIM_FAKE=1 "${SHIM}" --debug-output none > "${T}/shim22c.log" 2>&1 &
SHIM_PID=$!
wait_for_file "${REG}"
check "no syslog at all: the shim still starts and answers" "[ \"\$('${PROBE}' status)\" = 'GetCarLifeConnectStatus() = 0' ]"
stop_shim

echo "=== 23. a crash: logged (signal, build, PC), then death by the same signal (procguard restarts)"
start_shim "${T}/shim23.log" AA_SHIM_FAKE=1
kill -SEGV "${SHIM_PID}"
wait "${SHIM_PID}"
RC23=$?
check "dies by SIGSEGV (status 139), not exit 1" "[ ${RC23} -eq 139 ]" "${T}/shim23.log"
check "the crash is logged with the build id and the PC" \
      "grep -q 'crash: signal 11 (SIGSEGV), build src-' '${T}/shim23.log' && grep -q 'crash: pc 0x' '${T}/shim23.log'" "${T}/shim23.log"
rm -f "${REG}"

echo "=== 24. the Bluetooth lock while AA's phone holds the phone link; AA on screen for the Bluetooth app"
"${FAKE_BT}" --port 31333 --pub-port 31334 --hfp 11:22:33:44:55:66 --hfp-drop-ms 2000 > "${T}/bt24.log" 2>&1 &
BT_PID=$!
CTL24="${T}/ctl24.sock"
printf '10\n' > "${T}/btlock24"               # a lock left behind by an earlier run
start_shim "${T}/shim24.log" AA_BT_PORT=31333 AA_BT_LISTEN_PORT=31334 AA_SOURCE_PORT=0 AA_ILM_PORT=0 \
    AA_CAN_PORT=0 AA_CTL_SOCK="${CTL24}" AA_BT_LOCK_FILE="${T}/btlock24" AA_ONTOP_FILE="${T}/ontop24"
sleep 1.5                                   # subscribed to the events
check "an earlier run's lock taken off at the start, its file gone" \
      "grep -q 'SetCapability(10, 0)' '${T}/bt24.log' && [ ! -e '${T}/btlock24' ] && grep -q 'lock of an earlier run taken off' '${T}/shim24.log'" "${T}/shim24.log"
check "AA not on screen at the start: '<pid> 0'" "[ \"\$(cat '${T}/ontop24')\" = '${SHIM_PID} 0' ]" "${T}/shim24.log"
"${FAKE}" --sock "${CTL24}" connected projecting "line:btpair 11:22:33:44:55:66 2" > "${T}/fake24.log" 2>&1 &
FAKE_PID=$!
sleep 0.5
"${PROBE}" --allow-write connect > /dev/null  # the receiver's script: projecting, then the btpair
"${PROBE}" --allow-write show 1 > /dev/null
sleep 1.2
check "AA's phone holds the phone link: SetCapability(10, 1), the lock marked as ours" \
      "grep -q 'SetCapability(10, 1)' '${T}/bt24.log' && [ -e '${T}/btlock24' ] && grep -q 'holds the phone link: no other device' '${T}/shim24.log'" "${T}/shim24.log"
check "AA on screen: '<pid> 1' for the Bluetooth app" "[ \"\$(cat '${T}/ontop24')\" = '${SHIM_PID} 1' ]" "${T}/shim24.log"
"${PROBE}" --allow-write show 0 > /dev/null
check "AA hidden: '<pid> 0'" "[ \"\$(cat '${T}/ontop24')\" = '${SHIM_PID} 0' ]" "${T}/shim24.log"
sleep 2                                     # the phone link goes 2 s after the lock
check "the phone let go of the link: SetCapability(10, 0), the file gone" \
      "grep -q 'let go of the phone link' '${T}/shim24.log' && [ ! -e '${T}/btlock24' ] && [ \$(grep -c 'SetCapability(10, 0)' '${T}/bt24.log') -eq 2 ]" "${T}/shim24.log"
"${PROBE}" --allow-write show 1 > /dev/null
"${PROBE}" --allow-write disconnect > /dev/null
sleep 0.5
check "session over: AA not on screen" "[ \"\$(cat '${T}/ontop24')\" = '${SHIM_PID} 0' ]" "${T}/shim24.log"
stop_shim
check "  ... and nothing left locked at the stop" "[ ! -e '${T}/btlock24' ] && [ \$(grep -c 'SetCapability(10, 1)' '${T}/bt24.log') -ge 1 ]" "${T}/shim24.log"
kill "${BT_PID}" "${FAKE_PID}" 2>/dev/null; wait "${BT_PID}" "${FAKE_PID}" 2>/dev/null

echo
if [ "${fails}" -eq 0 ]; then echo "ALL PASSED"; else echo "${fails} FAILED (logs in ${T})"; fi
[ "${fails}" -eq 0 ]
