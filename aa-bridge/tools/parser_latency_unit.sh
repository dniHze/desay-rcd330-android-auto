#!/usr/bin/env bash
# Frame latency through the UNIT's own h264parse (GStreamer 1.2.3 from the device sysroot), with
# and without the access unit delimiter the bridge appends after each frame. The desktop smoke
# test cannot show this: GStreamer 1.22's h264parse decides where an access unit ends
# differently (from a complete next NAL, not from a look at its first bytes).
#
# The device build of the bridge (aa-bridge.armhf) runs on the unit's loader, glibc and
# GStreamer through qemu-arm -L, with "identity" in place of the ducati decoder and fakesink as
# the sink; a desktop feed_sender sends a canned stream one access unit per record, 250 ms apart,
# as Android Auto does while the screen changes now and then. It also checks, on the unit's
# GStreamer, what the desktop smoke test checks on a newer one: the drain sees EOS, a session
# without frames builds nothing, and a video error mid-session rebuilds video at the next
# keyframe while the session goes on. Run from the work tree:
#   docker run --rm --platform linux/arm64 -v "$PWD":/w debian:bookworm \
#       bash /w/aa-bridge/tools/parser_latency_unit.sh
set -u
W=/w
SYSROOT="${SYSROOT:-$W/.scratch/sysroot}"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq > /dev/null && apt-get install -y -qq --no-install-recommends qemu-user g++ \
    gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly > /dev/null 2>&1 || { echo "apt failed"; exit 1; }
T=$(mktemp -d); cd "$T"
g++ -std=gnu++11 -O2 -I"$W/aa-bridge/src" -o feed_sender "$W/aa-bridge/tools/feed_sender.cpp" \
    "$W/aa-bridge/src/feedproto.cpp" || exit 1
gst-launch-1.0 -q videotestsrc num-buffers=20 pattern=ball \
    ! video/x-raw,width=800,height=480,framerate=30/1 \
    ! x264enc tune=zerolatency key-int-max=30 ! video/x-h264,profile=baseline \
    ! h264parse ! video/x-h264,stream-format=byte-stream,alignment=au \
    ! filesink location=clip.h264 || exit 1

fails=0
run() { # $1 = AA_VIDEO_AUD, $2 = log
    rm -f sock
    GST_REGISTRY="$T/registry.$1.bin" GST_REGISTRY_FORK=no AA_VIDEO_AUD="$1" AA_MIC_SRC=none \
        qemu-arm -L "$SYSROOT" "$W/aa-bridge/aa-bridge.armhf" --socket "$T/sock" \
        --decoder identity --sink fakesink --once > "$2" 2>&1 &
    local pid=$!
    for _ in $(seq 1 300); do [ -S sock ] && break; sleep 0.1; done
    [ -S sock ] || { echo "bridge did not start:"; cat "$2"; return; }
    ./feed_sender --socket "$T/sock" --file clip.h264 --au --gap-ms 250 > /dev/null
    wait "$pid" 2>/dev/null
    grep -E '\] (pipeline|video |access unit)' "$2" | sed 's/^/    /'
}
max_parse_ms() { grep -oE 'push->parsed avg [0-9.]+' "$1" | awk '{ if ($3 > m) m = $3 } END { print m + 0 }'; }
echo "== unit h264parse, one access unit every 250 ms, delimiters on (the bridge's default)"
run 1 on.log
echo "== the same, delimiters off"
run 0 off.log
ON=$(max_parse_ms on.log); OFF=$(max_parse_ms off.log)
if grep -q 'video ' on.log && awk "BEGIN { exit !($ON < 50) }"; then
    echo "PASS: with delimiters a frame leaves the parser at once (avg up to $ON ms)"
else
    echo "FAIL: with delimiters, push->parsed avg up to $ON ms"; fails=$((fails+1))
fi
if awk "BEGIN { exit !($OFF > 150) }"; then
    echo "PASS: without them each frame waits for the next one (avg up to $OFF ms)"
else
    echo "FAIL: without delimiters, push->parsed avg up to $OFF ms"; fails=$((fails+1))
fi
# 21: the 20 frames, and at end of stream the delimiter that followed the last one, on its own.
N=$(grep -oE '[0-9]+ parsed' on.log | tail -1 | grep -oE '[0-9]+')
if [ "${N:-0}" -eq 21 ]; then
    echo "PASS: all 20 frames came out of the parser (and the last delimiter at end of stream)"
else
    echo "FAIL: ${N:-0} buffers came out of the parser, expected 21"; fails=$((fails+1))
fi
if grep -q 'pipeline reached EOS cleanly' on.log; then
    echo "PASS: the drain saw EOS on the unit's GStreamer"
else
    echo "FAIL: no EOS at the drain"; fails=$((fails+1))
fi

# $1 = log, then extra bridge options; the feed comes from the rest of the arguments after --.
bridge_run() {
    local log="$1"; shift
    local opts=()
    while [ "$1" != -- ]; do opts+=("$1"); shift; done; shift
    rm -f sock
    GST_REGISTRY="$T/registry.1.bin" GST_REGISTRY_FORK=no AA_MIC_SRC=none \
        qemu-arm -L "$SYSROOT" "$W/aa-bridge/aa-bridge.armhf" --socket "$T/sock" --sink fakesink --once \
        "${opts[@]}" > "$log" 2>&1 &
    local pid=$!
    for _ in $(seq 1 300); do [ -S sock ] && break; sleep 0.1; done
    ./feed_sender --socket "$T/sock" "$@" > /dev/null
    wait "$pid" 2>/dev/null
}
echo "== a session with VIDEO_START and no frame"
bridge_run empty.log --decoder identity -- --file clip.h264 --frames 0
if grep -q 'session over: video=0 ' empty.log && ! grep -q 'pipeline PLAYING' empty.log && ! grep -q ERROR empty.log; then
    echo "PASS: nothing built, nothing logged as an error"
else
    echo "FAIL: empty session"; sed 's/^/    /' empty.log; fails=$((fails+1))
fi
echo "== a video error after 5 frames (identity error-after=5), frames 100 ms apart, a keyframe every 10"
gst-launch-1.0 -q videotestsrc num-buffers=20 pattern=ball \
    ! video/x-raw,width=800,height=480,framerate=30/1 \
    ! x264enc tune=zerolatency key-int-max=10 ! video/x-h264,profile=baseline \
    ! h264parse ! video/x-h264,stream-format=byte-stream,alignment=au \
    ! filesink location=clip10.h264 || exit 1
bridge_run verr.log --decoder "identity error-after=5" -- --file clip10.h264 --au --gap-ms 100 --audio-ids 101
grep -E 'video ERROR|rebuilding|shim|session over' verr.log | sed 's/^/    /'
if grep -q 'video ERROR' verr.log && grep -q 'rebuilding the pipeline (keyframe)' verr.log \
        && grep -q 'asked the shim to show its surface' verr.log \
        && grep -q 'session over: video=20 audio=20 ' verr.log; then
    echo "PASS: video rebuilt at the next keyframe; the session and its audio went on"
else
    echo "FAIL: video error mid-session"; fails=$((fails+1))
fi
[ "$fails" -eq 0 ] && echo "ALL PASSED" || echo "$fails FAILED"
exit "$fails"
