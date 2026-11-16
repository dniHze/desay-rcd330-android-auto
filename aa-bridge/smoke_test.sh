#!/usr/bin/env bash
# Off-hardware smoke test for aa-bridge — validates socket + feedproto framing + GStreamer
# pipeline plumbing with a SOFTWARE decoder, independent of the ducati decoder / ILM surface.
#
# It: builds the desktop variant, runs the feedproto unit test, generates a canned H.264
# Annex-B stream with GStreamer (videotestsrc ! x264enc), starts aa-bridge (avdec_h264 !
# fakesink, --once), streams the file wrapped in feedproto via feed_sender, and asserts the
# bridge parsed >0 video frames and ran the pipeline with no error.
#
# Run it inside a Linux box/container that has (or can apt-get) GStreamer; e.g.:
#   docker run --rm --platform linux/arm64 -v "$PWD":/work debian:bookworm \
#       bash /work/aa-bridge/smoke_test.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SOCK="/tmp/aa.smoke.$$.sock"
H264="/tmp/aa.smoke.$$.h264"
LOG="/tmp/aa.smoke.$$.log"

# --- deps: install if gst tools are missing and apt is available -----------------------
if ! command -v gst-launch-1.0 >/dev/null 2>&1 || ! command -v g++ >/dev/null 2>&1; then
    if command -v apt-get >/dev/null 2>&1; then
        echo "==> installing build + gstreamer deps (apt)"
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y -qq --no-install-recommends \
            g++ pkg-config \
            libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
            gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
            gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav >/dev/null
    else
        echo "ERROR: need g++ and gst-launch-1.0 (no apt-get to install them)" >&2
        exit 1
    fi
fi

# --- 1) build desktop variant + unit test ----------------------------------------------
echo "==> building desktop variant"
bash "${HERE}/build.sh" desktop

# --- 2) feedproto framing unit test (C++ reader vs the Rust writer's layout) ------------
echo "==> feedproto unit test"
"${HERE}/feedproto_test"

# --- 3) canned H.264 Annex-B elementary stream -----------------------------------------
echo "==> generating canned H.264 with videotestsrc ! x264enc"
gst-launch-1.0 -q videotestsrc num-buffers=30 pattern=ball \
    ! video/x-raw,width=320,height=240,framerate=30/1 \
    ! x264enc tune=zerolatency key-int-max=15 \
    ! h264parse \
    ! video/x-h264,stream-format=byte-stream,alignment=au \
    ! filesink location="${H264}"
echo "    wrote ${H264} ($(wc -c < "${H264}") bytes)"
LONG="/tmp/aa.smoke.$$.long.h264" # 90 frames, a keyframe every 15, for the stall test
gst-launch-1.0 -q videotestsrc num-buffers=90 pattern=ball \
    ! video/x-raw,width=320,height=240,framerate=30/1 \
    ! x264enc tune=zerolatency key-int-max=15 \
    ! h264parse \
    ! video/x-h264,stream-format=byte-stream,alignment=au \
    ! filesink location="${LONG}"
ms() { echo $(( $(date +%s%N) / 1000000 )); }

# --- 4) run the bridge (software path) and feed it -------------------------------------
echo "==> starting aa-bridge (avdec_h264 ! fakesink, --once)"
rm -f "${SOCK}"
# `--debug-output none` is appended exactly as svp_start_service does on the unit: the
# bridge must accept it (it used to exit "unknown option" and never start on the device).
"${HERE}/aa-bridge" --socket "${SOCK}" --decoder avdec_h264 --sink fakesink --once --verbose \
    --audio-sink-for "102=fakesink silent=true" --debug-output none \
    > "${LOG}" 2>&1 &
BRIDGE_PID=$!

# wait for the bridge to bind the socket
for _ in $(seq 1 50); do
    [ -S "${SOCK}" ] && break
    sleep 0.1
done
if [ ! -S "${SOCK}" ]; then
    echo "ERROR: bridge did not create ${SOCK}" >&2
    cat "${LOG}" || true
    kill "${BRIDGE_PID}" 2>/dev/null || true
    exit 1
fi

echo "==> feeding the canned stream over feedproto"
"${HERE}/feed_sender" --socket "${SOCK}" --file "${H264}" --codec h264 --audio-ids 101,102

# wait for the bridge (--once) to finish its session and exit: the drain sees EOS at once
T0=$(ms)
wait "${BRIDGE_PID}" 2>/dev/null || true
DRAIN_MS=$(( $(ms) - T0 ))

echo "----------------------------------------------------------------"
echo "aa-bridge output:"
cat "${LOG}"
echo "----------------------------------------------------------------"

# --- 5) assertions ---------------------------------------------------------------------
STATUS=0
if grep -q "pipeline PLAYING" "${LOG}"; then
    echo "PASS: pipeline reached PLAYING"
else
    echo "FAIL: pipeline never reached PLAYING"; STATUS=1
fi

FRAMES="$(grep -oE 'session over: video=[0-9]+' "${LOG}" | grep -oE '[0-9]+' | head -1 || echo 0)"
if [ "${FRAMES:-0}" -gt 0 ]; then
    echo "PASS: bridge parsed ${FRAMES} video frames from feedproto"
else
    echo "FAIL: bridge parsed 0 video frames"; STATUS=1
fi

# Two overlapping audio streams, each on its own sink (the per-id routing used on the unit).
if grep -q "audio id=101 PLAYING (48000Hz/2ch -> fakesink)" "${LOG}" \
        && grep -q "audio id=102 PLAYING (16000Hz/1ch -> fakesink silent=true)" "${LOG}"; then
    echo "PASS: audio ids 101 and 102 playing concurrently on their own sinks"
else
    echo "FAIL: audio streams did not both start on their configured sinks"; STATUS=1
fi

if grep -q "(ERROR)" "${LOG}" || grep -q "pipeline ERROR" "${LOG}" || grep -q "video ERROR" "${LOG}"; then
    echo "FAIL: pipeline reported an error"; STATUS=1
else
    echo "PASS: no pipeline error"
fi

if grep -q "pipeline reached EOS cleanly" "${LOG}" && [ "${DRAIN_MS}" -lt 1000 ]; then
    echo "PASS: the drain saw EOS; the session ended ${DRAIN_MS} ms after the feed closed"
else
    echo "FAIL: drain (EOS seen: $(grep -c 'reached EOS cleanly' "${LOG}" || true), ${DRAIN_MS} ms after the feed closed)"; STATUS=1
fi

# --- 6) frames sent as Android Auto sends them -------------------------------------------
# One access unit per record, 250 ms apart (AA sends nothing while the screen is still). Without
# a delimiter after each frame h264parse holds every frame until the next one arrives (the gap),
# as the unit's did. With them the stream must still decode cleanly. Whether they let the frame
# go at once depends on the h264parse version: the unit's 1.2.3 does (tools/
# parser_latency_unit.sh runs it), this container's newer one waits for the delimiter to end too.
latency_run() { # $1 = AA_VIDEO_AUD, $2 = log
    rm -f "${SOCK}"
    AA_VIDEO_AUD="$1" "${HERE}/aa-bridge" --socket "${SOCK}" --decoder avdec_h264 --sink fakesink \
        --once > "$2" 2>&1 &
    local pid=$!
    for _ in $(seq 1 50); do [ -S "${SOCK}" ] && break; sleep 0.1; done
    "${HERE}/feed_sender" --socket "${SOCK}" --file "${H264}" --codec h264 --au --gap-ms 250 > /dev/null
    wait "${pid}" 2>/dev/null || true
    grep '\] video ' "$2" | sed 's/^/    /' || true
}
max_parse_ms() { grep -oE 'push->parsed avg [0-9.]+' "$1" | awk '{ if ($3 > m) m = $3 } END { print m + 0 }'; }
echo "==> latency, one access unit per record every 250 ms, delimiters on"
latency_run 1 "${LOG}.aud"
echo "==> the same, delimiters off"
latency_run 0 "${LOG}.noaud"
ON="$(max_parse_ms "${LOG}.aud")"; OFF="$(max_parse_ms "${LOG}.noaud")"
if grep -q 'delimiter after each frame: on' "${LOG}.aud"; then
    echo "PASS: delimiters on by default (push->parsed avg up to ${ON} ms with this h264parse)"
else
    echo "FAIL: delimiters not on by default"; STATUS=1
fi
if awk "BEGIN { exit !(${OFF} > 150) }"; then
    echo "PASS: without them it waits for the next frame (avg up to ${OFF} ms), the lag seen on the unit"
else
    echo "FAIL: without delimiters, push->parsed avg up to ${OFF} ms (expected about the 250 ms gap)"; STATUS=1
fi
if grep -q "(ERROR)" "${LOG}.aud" || grep -q "pipeline ERROR" "${LOG}.aud" || grep -q "video ERROR" "${LOG}.aud"; then
    echo "FAIL: the stream with delimiters made the pipeline fail"; STATUS=1
fi
DEC="$(grep -oE '[0-9]+ decoded' "${LOG}.aud" | tail -1 | grep -oE '[0-9]+' || echo 0)"
if [ "${DEC:-0}" -ge 25 ]; then
    echo "PASS: the decoder decoded ${DEC} of the 30 frames sent with delimiters"
else
    echo "FAIL: only ${DEC:-0} frames decoded with delimiters"; STATUS=1
fi

# --- 7) speech through PulseAudio's simple API ------------------------------------------
# The unit's GStreamer pulse plugin is broken, so speech (102) plays through libpulse-simple
# ("pasimple"), under the stock prompt stream name, against a system-mode daemon as on the unit.
echo "==> speech (id 102) through a system PulseAudio daemon (pasimple)"
if command -v pulseaudio > /dev/null 2>&1 || { command -v apt-get > /dev/null 2>&1 \
        && apt-get install -y -qq --no-install-recommends pulseaudio pulseaudio-utils > /dev/null 2>&1; }; then
    pulseaudio --system --daemonize --disallow-exit --disallow-module-loading -n \
        --load="module-native-protocol-unix auth-anonymous=1" --load=module-null-sink > /dev/null 2>&1
    for _ in $(seq 1 50); do [ -S /var/run/pulse/native ] && break; sleep 0.1; done
    rm -f "${SOCK}"
    "${HERE}/aa-bridge" --socket "${SOCK}" --decoder avdec_h264 --sink fakesink --once \
        --audio-sink-for "102=pasimple" > "${LOG}.pa" 2>&1 &
    PID=$!
    for _ in $(seq 1 50); do [ -S "${SOCK}" ] && break; sleep 0.1; done
    # The feed stays up until the sound has played out (a fresh daemon's first stream can block
    # its first write for over a second), so the stream closes on its own rather than at the
    # session end.
    "${HERE}/feed_sender" --socket "${SOCK}" --file "${H264}" --codec h264 --audio-ids 102 --hold-ms 3000 > /dev/null
    wait "${PID}" 2>/dev/null || true
    grep 'audio id=102' "${LOG}.pa" | sed 's/^/    /'
    if grep -q "audio id=102 PLAYING (16000Hz/1ch -> pulse stream 'sv-as-carlife-prompt', upsampled to 48000Hz here, 200 ms buffer)" "${LOG}.pa" \
            && grep -qE "pulse stream 'sv-as-carlife-prompt' closed after [1-9][0-9]* bytes" "${LOG}.pa"; then
        echo "PASS: speech played as the 'sv-as-carlife-prompt' stream"
    else
        echo "FAIL: speech did not play through PulseAudio"; STATUS=1
    fi
    # One line per sound: its length, the waits for data, its peak and the samples limited.
    if grep -qE "audio id=102 sound ended: 0\.[1-9][0-9] s of audio over [0-9.]+ s, longest wait for data [0-9]+ ms \(0 longer than the 200 ms buffer\), peak -?[0-9.]+ dBFS, 0 samples limited" "${LOG}.pa"; then
        echo "PASS: the sound's line (length, waits for data, peak, samples limited)"
    else
        echo "FAIL: no line for the sound"; STATUS=1
    fi
    # Held open ("pasimple-open"): the stream exists from the start of the session, before any
    # sound, and stays open, silent, after it (the unit's audiomgr sets the navigation volume
    # only on streams that exist when the source becomes active).
    rm -f "${SOCK}"
    "${HERE}/aa-bridge" --socket "${SOCK}" --decoder avdec_h264 --sink fakesink --once \
        --audio-sink-for "102=pasimple-open" > "${LOG}.pah" 2>&1 &
    PID=$!
    for _ in $(seq 1 50); do [ -S "${SOCK}" ] && break; sleep 0.1; done
    # The feed stays up past the end of the sound: the session's end no longer waits 2 s for a
    # drain that never saw its EOS, and the held stream starts the sound behind the silence
    # it has queued (about 1.9 s with this daemon).
    "${HERE}/feed_sender" --socket "${SOCK}" --file "${H264}" --codec h264 --audio-ids 102 --hold-ms 4000 \
        > /dev/null &
    SENDER=$!
    sleep 2
    INPUTS="$(PULSE_SERVER=unix:/var/run/pulse/native pactl list sink-inputs 2>&1 | grep -c 'media.name = "sv-as-carlife-prompt"')"
    wait "${SENDER}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
    grep 'audio id=102' "${LOG}.pah" | sed 's/^/    /'
    OPEN_LINE="$(grep -n "pulse stream 'sv-as-carlife-prompt' open for the session" "${LOG}.pah" | head -1 | cut -d: -f1)"
    PLAY_LINE="$(grep -n "audio id=102 PLAYING" "${LOG}.pah" | head -1 | cut -d: -f1)"
    if [ -n "${OPEN_LINE}" ] && [ -n "${PLAY_LINE}" ] && [ "${OPEN_LINE}" -lt "${PLAY_LINE}" ] \
            && grep -qE "sound ended \(silence until the next\): 0\.[1-9][0-9] s of audio" "${LOG}.pah" && [ "${INPUTS}" -eq 1 ] \
            && [ "$(grep -c "pulse stream 'sv-as-carlife-prompt' open for the session" "${LOG}.pah")" -eq 1 ]; then
        echo "PASS: held speech stream open before the sound, still open (silent) after it, once"
    else
        echo "FAIL: held speech stream (open line ${OPEN_LINE:-none}, PLAYING line ${PLAY_LINE:-none}, ${INPUTS} sink inputs while idle)"; STATUS=1
    fi
    # No daemon at all: one attempt a second and one line, not one per chunk of sound.
    rm -f "${SOCK}"
    PULSE_SERVER=unix:/nonexistent "${HERE}/aa-bridge" --socket "${SOCK}" --decoder avdec_h264 --sink fakesink \
        --once --audio-sink-for "102=pasimple" > "${LOG}.panone" 2>&1 &
    PID=$!
    for _ in $(seq 1 50); do [ -S "${SOCK}" ] && break; sleep 0.1; done
    "${HERE}/feed_sender" --socket "${SOCK}" --file "${H264}" --codec h264 --audio-ids 102 > /dev/null
    wait "${PID}" 2>/dev/null || true
    FAILED="$(grep -c "pulse stream 'sv-as-carlife-prompt' failed" "${LOG}.panone" || true)"
    if [ "${FAILED}" -eq 1 ]; then
        echo "PASS: no PulseAudio: the failure is logged once for 20 chunks of sound"
    else
        echo "FAIL: no PulseAudio: ${FAILED} failure lines for 20 chunks"; STATUS=1
    fi
    # A daemon that stops answering (SIGSTOP) must not hold the session end.
    rm -f "${SOCK}"
    "${HERE}/aa-bridge" --socket "${SOCK}" --decoder avdec_h264 --sink fakesink --once \
        --audio-sink-for "102=pasimple" > "${LOG}.pastop" 2>&1 &
    PID=$!
    for _ in $(seq 1 50); do [ -S "${SOCK}" ] && break; sleep 0.1; done
    "${HERE}/feed_sender" --socket "${SOCK}" --file "${H264}" --codec h264 --audio-ids 102 --hold-ms 1500 \
        > /dev/null &
    SENDER=$!
    sleep 0.8
    pkill -STOP pulseaudio 2>/dev/null || true
    wait "${SENDER}" 2>/dev/null || true
    T0=$(ms)
    while kill -0 "${PID}" 2>/dev/null && [ $(( $(ms) - T0 )) -lt 8000 ]; do sleep 0.1; done
    END_MS=$(( $(ms) - T0 ))
    kill "${PID}" 2>/dev/null || true
    pkill -CONT pulseaudio 2>/dev/null || true
    grep -E "audio id=102 .*(closed|left behind)" "${LOG}.pastop" | sed 's/^/    /' || true
    if [ "${END_MS}" -lt 4000 ] && grep -q "session over" "${LOG}.pastop"; then
        echo "PASS: PulseAudio stopped: the session still ended, ${END_MS} ms after the feed closed"
    else
        echo "FAIL: PulseAudio stopped: the bridge was still in its session end after ${END_MS} ms"; STATUS=1
    fi
    pkill pulseaudio 2>/dev/null || true
else
    echo "SKIP: no pulseaudio to test against"
fi

# --- 8) microphone through libasound ("alsa:<device>") ---------------------------------
# As the unit's CarLife service records: snd_pcm on a plug device over a 48 kHz capture. Here
# the capture is an ALSA file device playing a 440 Hz tone; a stand-in for the receiver binds
# the mic socket, takes 2 s of records and closes, as the receiver does when the phone is done.
echo "==> microphone through libasound (alsa:<device>)"
gst-launch-1.0 -q audiotestsrc num-buffers=300 freq=440 volume=0.5 \
    ! audio/x-raw,format=S16LE,rate=48000,channels=1 ! filesink location=/tmp/mic.raw
cat > "${HOME}/.asoundrc" <<'ASOUND'
pcm.testmic {
    type plug
    slave {
        pcm { type file slave.pcm "null" file "/dev/null" infile "/tmp/mic.raw" format "raw" }
        format S16_LE
        rate 48000
        channels 1
    }
}
ASOUND
cat > /tmp/micsink.c <<'MICSINK'
/* Binds the mic socket like the receiver, reads for 2 s, prints the bytes, closes. */
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
int main(int argc, char **argv) {
    struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;
    strncpy(a.sun_path, argv[1], sizeof a.sun_path - 1); unlink(argv[1]);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bind(s, (struct sockaddr *) &a, sizeof a) || listen(s, 1)) return 1;
    int c = accept(s, 0, 0); long total = 0; char b[4096];
    time_t end = time(0) + 2;
    while (time(0) < end) {
        struct pollfd p = {c, POLLIN, 0};
        if (poll(&p, 1, 100) > 0) { ssize_t n = read(c, b, sizeof b); if (n <= 0) break; total += n; }
    }
    printf("micsink: %ld bytes\n", total); close(c); unlink(argv[1]); return 0;
}
MICSINK
gcc -o /tmp/micsink /tmp/micsink.c
rm -f "${SOCK}"
"${HERE}/aa-bridge" --socket "${SOCK}" --decoder avdec_h264 --sink fakesink --once \
    --mic-src alsa:testmic --mic-sock /tmp/aa.smoke.mic.sock > "${LOG}.mic" 2>&1 &
PID=$!
/tmp/micsink /tmp/aa.smoke.mic.sock > "${LOG}.micsink" 2>&1
sleep 1
for _ in $(seq 1 50); do [ -S "${SOCK}" ] && break; sleep 0.1; done
"${HERE}/feed_sender" --socket "${SOCK}" --file "${H264}" --codec h264 > /dev/null
wait "${PID}" 2>/dev/null || true
grep 'mic:' "${LOG}.mic" | sed 's/^/    /'; sed 's/^/    /' "${LOG}.micsink"
PEAK="$(grep -oE 'peak level [0-9]+' "${LOG}.mic" | head -1 | grep -oE '[0-9]+$' || echo 0)"
if grep -q 'mic: testmic open (16000Hz/1ch S16LE)' "${LOG}.mic" && [ "${PEAK:-0}" -gt 1000 ] \
        && grep -qE 'micsink: [1-9][0-9]{4,} bytes' "${LOG}.micsink"; then
    echo "PASS: the mic captured the 48 kHz tone at 16 kHz mono and sent it (peak ${PEAK})"
else
    echo "FAIL: mic capture through libasound"; STATUS=1
fi

# --- 9) a stale session does not block the next one -------------------------------------
# The receiver can lose track of a session (the phone left accessory mode and came straight
# back): its feed connection stays open. A newer feed connection must end it, not wait.
echo "==> a newer feed connection ends a stale session"
rm -f "${SOCK}"
"${HERE}/aa-bridge" --socket "${SOCK}" --decoder avdec_h264 --sink fakesink > "${LOG}.stale" 2>&1 &
PID=$!
for _ in $(seq 1 50); do [ -S "${SOCK}" ] && break; sleep 0.1; done
"${HERE}/feed_sender" --socket "${SOCK}" --file "${H264}" --codec h264 --hold-ms 20000 > /dev/null &
STALE=$!
sleep 1
"${HERE}/feed_sender" --socket "${SOCK}" --file "${H264}" --codec h264 > /dev/null
sleep 3
kill "${STALE}" "${PID}" 2>/dev/null; wait "${STALE}" "${PID}" 2>/dev/null || true
grep -E 'feed connection accepted|newer feed|session over' "${LOG}.stale" | sed 's/^/    /'
if grep -q 'a newer feed connection is waiting: ending this session' "${LOG}.stale" \
        && [ "$(grep -c 'feed connection accepted' "${LOG}.stale")" -eq 2 ] \
        && [ "$(grep -c 'session over: video=' "${LOG}.stale")" -ge 2 ]; then
    echo "PASS: the newer session took over the feed while the stale one was still open"
else
    echo "FAIL: a stale feed connection blocked the next session"; STATUS=1
fi

# --- 10) a session that never sends a frame ------------------------------------------------
# The receiver's first session after the phone switches often dies right after its setup: a
# VIDEO_START and nothing else. No decoder is built for it, and nothing is logged as an error.
echo "==> a session with VIDEO_START and no frame"
rm -f "${SOCK}"
"${HERE}/aa-bridge" --socket "${SOCK}" --decoder avdec_h264 --sink fakesink --once > "${LOG}.empty" 2>&1 &
PID=$!
for _ in $(seq 1 50); do [ -S "${SOCK}" ] && break; sleep 0.1; done
"${HERE}/feed_sender" --socket "${SOCK}" --file "${H264}" --codec h264 --frames 0 > /dev/null
wait "${PID}" 2>/dev/null || true
if grep -q "VIDEO_START" "${LOG}.empty" && ! grep -q "pipeline PLAYING" "${LOG}.empty" \
        && grep -q "session over: video=0 " "${LOG}.empty" && ! grep -q "ERROR" "${LOG}.empty"; then
    echo "PASS: no pipeline built and no error for a session without frames"
else
    echo "FAIL: empty session"; sed 's/^/    /' "${LOG}.empty"; STATUS=1
fi

# --- 11) a video error in the middle of a session --------------------------------------------
# The decoder fails after 10 frames: video stops, is rebuilt at the next keyframe (frame 15) and
# fails again at 25; the session and its audio go on to the end of the feed.
echo "==> a video error mid-session (identity error-after=10 ahead of the decoder)"
rm -f "${SOCK}"
"${HERE}/aa-bridge" --socket "${SOCK}" --decoder "identity error-after=10 ! avdec_h264" --sink fakesink \
    --once > "${LOG}.verr" 2>&1 &
PID=$!
for _ in $(seq 1 50); do [ -S "${SOCK}" ] && break; sleep 0.1; done
"${HERE}/feed_sender" --socket "${SOCK}" --file "${H264}" --codec h264 --au --gap-ms 50 --audio-ids 101,102 > /dev/null
wait "${PID}" 2>/dev/null || true
grep -E "video ERROR|rebuilding|shim|session over|audio id=10[12] PLAYING" "${LOG}.verr" | sed 's/^/    /' || true
if [ "$(grep -c "video ERROR" "${LOG}.verr" || true)" -ge 1 ] \
        && grep -q "rebuilding the pipeline (keyframe)" "${LOG}.verr" \
        && [ "$(grep -c "pipeline PLAYING" "${LOG}.verr" || true)" -ge 2 ] \
        && grep -q "asked the shim to show its surface" "${LOG}.verr" \
        && grep -q "audio id=101 PLAYING" "${LOG}.verr" \
        && grep -qE "session over: video=30 audio=40 " "${LOG}.verr" \
        && [ "$(grep -c "session over" "${LOG}.verr" || true)" -eq 1 ]; then
    echo "PASS: video rebuilt at the next keyframe (the shim told); the feed and all 40 audio records went on"
else
    echo "FAIL: video error mid-session"; STATUS=1
fi

# --- 12) a sink that stops taking frames -------------------------------------------------------
# appsink with nobody pulling holds after its first frames: a stall with no error. It is found
# after 5 s, and video is rebuilt at the next keyframe.
echo "==> a sink that stalls without an error (appsink max-buffers=1, never pulled)"
rm -f "${SOCK}"
"${HERE}/aa-bridge" --socket "${SOCK}" --decoder avdec_h264 --sink "appsink max-buffers=1 drop=false" \
    --once > "${LOG}.stall" 2>&1 &
PID=$!
for _ in $(seq 1 50); do [ -S "${SOCK}" ] && break; sleep 0.1; done
"${HERE}/feed_sender" --socket "${SOCK}" --file "${LONG}" --codec h264 --au --gap-ms 150 > /dev/null
wait "${PID}" 2>/dev/null || true
grep -E "stalled|rebuilding|session over" "${LOG}.stall" | sed 's/^/    /' || true
if grep -q "video ERROR: sink stalled" "${LOG}.stall" && grep -q "rebuilding the pipeline" "${LOG}.stall" \
        && grep -q "session over: video=90 " "${LOG}.stall"; then
    echo "PASS: the stall was found and video rebuilt; the session went on"
else
    echo "FAIL: sink stall"; STATUS=1
fi

# --- 13) a corrupt feed ---------------------------------------------------------------------------
# A record length no record has: the rest of the stream cannot be framed, so the session ends.
echo "==> a corrupt record length ends the session"
rm -f "${SOCK}"
"${HERE}/aa-bridge" --socket "${SOCK}" --decoder avdec_h264 --sink fakesink --once > "${LOG}.corrupt" 2>&1 &
PID=$!
for _ in $(seq 1 50); do [ -S "${SOCK}" ] && break; sleep 0.1; done
"${HERE}/feed_sender" --socket "${SOCK}" --file "${H264}" --codec h264 --corrupt-after 5 > /dev/null 2>&1 || true
wait "${PID}" 2>/dev/null || true
if grep -q "the stream is corrupt; ending the session" "${LOG}.corrupt" \
        && grep -q "session over: video=5 .*feed corrupt (ERROR)" "${LOG}.corrupt"; then
    echo "PASS: the session ended at the corrupt record"
else
    echo "FAIL: corrupt feed"; sed 's/^/    /' "${LOG}.corrupt"; STATUS=1
fi

# --- 14) SIGTERM ------------------------------------------------------------------------------------
# In a session and while idle: a clean stop, exit status 0 (procguard restarts), within a second.
echo "==> SIGTERM in a session and while idle"
rm -f "${SOCK}"
"${HERE}/aa-bridge" --socket "${SOCK}" --decoder avdec_h264 --sink fakesink > "${LOG}.term" 2>&1 &
PID=$!
for _ in $(seq 1 50); do [ -S "${SOCK}" ] && break; sleep 0.1; done
"${HERE}/feed_sender" --socket "${SOCK}" --file "${H264}" --codec h264 --hold-ms 5000 > /dev/null 2>&1 &
SENDER=$!
sleep 1
T0=$(ms)
kill -TERM "${PID}"
RC=0; wait "${PID}" || RC=$?
TERM_MS=$(( $(ms) - T0 ))
kill "${SENDER}" 2>/dev/null || true
wait "${SENDER}" 2>/dev/null || true
rm -f "${SOCK}"
"${HERE}/aa-bridge" --socket "${SOCK}" --decoder avdec_h264 --sink fakesink > "${LOG}.termidle" 2>&1 &
PID=$!
for _ in $(seq 1 50); do [ -S "${SOCK}" ] && break; sleep 0.1; done
T0=$(ms)
kill -TERM "${PID}"
RC2=0; wait "${PID}" || RC2=$?
IDLE_MS=$(( $(ms) - T0 ))
if [ "${RC}" -eq 0 ] && [ "${TERM_MS}" -lt 1500 ] && grep -q "signal 15: stopping" "${LOG}.term" \
        && grep -q "session over" "${LOG}.term" && [ "${RC2}" -eq 0 ] && [ "${IDLE_MS}" -lt 1500 ] \
        && [ ! -e "${SOCK}" ]; then
    echo "PASS: SIGTERM: exit 0 after ${TERM_MS} ms in a session, ${IDLE_MS} ms idle; socket removed"
else
    echo "FAIL: SIGTERM (session: rc ${RC}, ${TERM_MS} ms; idle: rc ${RC2}, ${IDLE_MS} ms)"; STATUS=1
fi

# --- 15) the watchdog ------------------------------------------------------------------------------
# A decoder that sleeps 6 s on a frame holds the pipeline's stop (after the 2 s drain) for
# about 4 s: with AA_WATCHDOG_S=1 the bridge must leave (exit 0, for procguard to restart it)
# instead of waiting.
echo "==> watchdog: a stop that hangs (identity sleep-time=6000000, AA_WATCHDOG_S=1)"
rm -f "${SOCK}"
AA_WATCHDOG_S=1 "${HERE}/aa-bridge" --socket "${SOCK}" --decoder "identity sleep-time=6000000 ! avdec_h264" \
    --sink fakesink --once > "${LOG}.wd" 2>&1 &
PID=$!
for _ in $(seq 1 50); do [ -S "${SOCK}" ] && break; sleep 0.1; done
"${HERE}/feed_sender" --socket "${SOCK}" --file "${H264}" --codec h264 --frames 10 > /dev/null
RC=0; wait "${PID}" || RC=$?
grep "watchdog" "${LOG}.wd" | sed 's/^/    /' || true
if [ "${RC}" -eq 0 ] && grep -q "watchdog: the main loop has been stuck in video stop" "${LOG}.wd" \
        && ! grep -q "session over" "${LOG}.wd"; then
    echo "PASS: the watchdog ended the stuck bridge with exit 0"
else
    echo "FAIL: watchdog (rc ${RC})"; STATUS=1
fi

rm -f "${SOCK}" "${H264}" "${LONG}" "${LOG}" "${LOG}".* /tmp/aa.smoke.mic.sock
if [ "${STATUS}" -eq 0 ]; then
    echo "==> SMOKE TEST PASSED"
else
    echo "==> SMOKE TEST FAILED"
fi
exit "${STATUS}"
