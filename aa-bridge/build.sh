#!/usr/bin/env bash
# Build aa-bridge.
#
#   ./build.sh device    (default)  cross-build the armhf device binary + run the ABI gate
#   ./build.sh desktop              native build (software decoder) + feed_sender + unit test
#
# CROSS MODEL — identical to androidautoservice/build.sh (verified to emit GLIBC_2.4, well
# under the device's 2.15):
#   * HEADERS + base libc/libstdc++: the TOOLCHAIN's own sysroot (glibc 2.12, static libstdc++).
#     We do NOT pass --sysroot at the device tree.
#   * C++ always: -D_GLIBCXX_USE_CXX11_ABI=0, and -static-libstdc++ -static-libgcc so no
#     GLIBCXX_ version is demanded from the device at all.
#   * DEVICE libraries (gstreamer-1.0 / gstapp-1.0): pulled with -L + -Wl,-rpath-link against
#     the device sysroot.
#
# GSTREAMER HEADERS: the device sysroot ships GStreamer 1.2.3 + GLib 2.38 as .so only (no
# dev headers). Instead of overlaying mismatched modern headers, the bridge declares the
# exact 1.0-era C entry points it uses in src/gst_min.h (see the rationale there). We link
# against the real device .so (-l:libgstreamer-1.0.so.0 -l:libgstapp-1.0.so.0), so any
# accidentally-too-new symbol would fail the link. The ducati decoder + any sink are
# resolved at RUNTIME by the GStreamer registry (plugins), not at link time.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "${HERE}/.." && pwd)"
TARGET="${1:-device}"

SRCS=("${HERE}/src/feedproto.cpp" "${HERE}/src/pipeline.cpp" "${HERE}/src/video.cpp" "${HERE}/src/audio.cpp"
    "${HERE}/src/pa_out.cpp" "${HERE}/src/mic.cpp" "${HERE}/src/notify.cpp" "${HERE}/src/watchdog.cpp"
    "${HERE}/src/ilm_crop.cpp" "${HERE}/src/main.cpp")

# The unit's loader and glibc 2.15 must be able to run the binary, or procguard sees it exit
# non-zero and leaves it down for the boot. The same rules as aa-carlife-shim/build.sh; any
# breach fails the build.
abi_gate() {
    local bin="$1" readelf="$2"
    local interp maxglibc maxglibcxx
    interp="$("${readelf}" -l "${bin}" | grep -o '/lib/ld[^]]*' | head -1 || true)"
    maxglibc="$("${readelf}" -V "${bin}" | grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1 || true)"
    maxglibcxx="$("${readelf}" -V "${bin}" | grep -oE 'GLIBCXX_[0-9.]+' | sort -V | tail -1 || true)"
    echo "---- ABI gate: $(basename "${bin}") ----"
    echo "interp:    ${interp:-<none>}   (must be /lib/ld-linux-armhf.so.3)"
    echo "max GLIBC: ${maxglibc:-<none>}   (must be <= GLIBC_2.15)"
    echo "GLIBCXX:   ${maxglibcxx:-(none / static)}"
    echo "NEEDED:    $("${readelf}" -d "${bin}" | awk '/NEEDED/ {gsub(/[\[\]]/, "", $5); printf "%s ", $5}')"
    [ "${interp}" = "/lib/ld-linux-armhf.so.3" ] || { echo "ABI gate FAILED: interpreter" >&2; exit 1; }
    [ -z "${maxglibcxx}" ] || { echo "ABI gate FAILED: GLIBCXX dependency" >&2; exit 1; }
    if "${readelf}" -d "${bin}" | grep -q '\[ld-linux\.so\.3\]'; then
        echo "ABI gate FAILED: NEEDED ld-linux.so.3 (stock binaries use ld-linux-armhf.so.3)" >&2; exit 1
    fi
    if [ "$("${readelf}" -d "${bin}" | awk '/NEEDED/ {n=$5} END {print n}')" != "[ld-linux-armhf.so.3]" ]; then
        echo "ABI gate FAILED: ld-linux-armhf.so.3 must be the last NEEDED entry, after libc" >&2; exit 1
    fi
    if [ -n "${maxglibc}" ] && [ "$(printf '%s\nGLIBC_2.15\n' "${maxglibc}" | sort -V | tail -1)" != "GLIBC_2.15" ]; then
        echo "ABI gate FAILED: needs ${maxglibc}" >&2; exit 1
    fi
    echo "ABI gate passed"
}

if [ "${TARGET}" = "device" ]; then
    SYSROOT="${SYSROOT:-${WORK}/.scratch/sysroot}"
    CXX="${CXX:-${WORK}/.scratch/x-tools/arm-cortex_a15-linux-gnueabihf/bin/arm-cortex_a15-linux-gnueabihf-g++}"
    READELF="${READELF:-${WORK}/.scratch/x-tools/arm-cortex_a15-linux-gnueabihf/bin/arm-cortex_a15-linux-gnueabihf-readelf}"
    STRIP="${STRIP:-${WORK}/.scratch/x-tools/arm-cortex_a15-linux-gnueabihf/bin/arm-cortex_a15-linux-gnueabihf-strip}"
    [ -x "${CXX}" ] || { echo "CXX not found/executable: ${CXX}" >&2; exit 1; }
    [ -d "${SYSROOT}" ] || { echo "SYSROOT not found: ${SYSROOT}" >&2; exit 1; }

    CXXFLAGS="-std=gnu++11 -O2 -D_GLIBCXX_USE_CXX11_ABI=0 -static-libstdc++ -static-libgcc -fno-rtti"
    # Audio as the unit's own CarLife service plays it (carlifeservice CMediaPlayer /
    # CMicrophone): media (101) on the ALSA default (plug:hw:MIB2HiFi, no dmix); the alert
    # streams (102 speech / 103 system / 104 telephony) as PulseAudio streams named
    # "sv-as-carlife-prompt" through libpulse-simple ("pasimple", see pa_out.h; the unit's
    # GStreamer pulse plugin is broken), mixed by the system daemon on hw:MIB2AuxSound, each
    # stream reported to the shim to register with audiomgr for its volume; the mic
    # from MIB2BtOptIn (asound.conf: plug over hw:MIB2BtOpt, the BT module's capture) through
    # libasound directly ("alsa:", see mic.h; the unit's alsasrc does not negotiate). Override
    # with AA_AUDIO_SINK / AA_AUDIO_SINKS / AA_MIC_SRC in the environment, no rebuild needed.
    # The sink is waylandsink2, as the launcher passes and as the CarLife shim's default
    # AA_VIDEO_SOURCE (32,24,800,480) assumes: a missing --sink must not change the picture.
    # An array, so that each define reaches the compiler as one word with its quotes.
    DEVICE_DEFS=('-DAA_DEFAULT_DECODER="ducatih264dec"' '-DAA_DEFAULT_SINK="waylandsink2"'
        '-DAA_DEFAULT_AUDIO_SINK="alsasink"' '-DAA_DEFAULT_AUDIO_SINKS="102=pasimple,103=pasimple,104=pasimple"'
        '-DAA_DEFAULT_MIC_SRC="alsa:MIB2BtOptIn"')
    INCLUDES="-I${HERE}/src"
    LIBDIRS="-L${SYSROOT}/usr/lib -L${SYSROOT}/lib -L${SYSROOT}/svp/lib"
    RPATHLINK="-Wl,-rpath-link,${SYSROOT}/usr/lib:${SYSROOT}/lib:${SYSROOT}/svp/lib"
    GST_LIBS="-l:libgstreamer-1.0.so.0 -l:libgstapp-1.0.so.0"

    # Interposed for the unit's plugins (see each file): wl_dispatch.cpp, a time-limited
    # wl_display_dispatch for its waylandsink; dce_params.cpp, VIDDEC3_create with the H.264
    # display delay of the unit's own player, for gst-ducati. --dynamic-list exports just those
    # symbols, so the plugins' PLTs resolve them here.
    DEVICE_SRCS=("${SRCS[@]}" "${HERE}/src/wl_dispatch.cpp" "${HERE}/src/dce_params.cpp")
    EXPORTS="-Wl,--dynamic-list=${HERE}/src/device_exports.list -ldl"

    # aa-bridge.unstripped.armhf keeps symbols for on-device gdb; the stripped aa-bridge.armhf
    # is what goes into the image (it has to fit on the read-only /svp partition).
    OUT_FULL="${HERE}/aa-bridge.unstripped.armhf"
    OUT="${HERE}/aa-bridge.armhf"
    echo "==> device build: ${OUT}"
    # shellcheck disable=SC2086
    "${CXX}" ${CXXFLAGS} "${DEVICE_DEFS[@]}" ${INCLUDES} "${DEVICE_SRCS[@]}" \
        ${LIBDIRS} ${RPATHLINK} ${GST_LIBS} ${EXPORTS} -lpthread -lrt -o "${OUT_FULL}"
    "${STRIP}" -o "${OUT}" "${OUT_FULL}"
    echo "built: ${OUT} (stripped; symbols in ${OUT_FULL})"
    file "${OUT}" || true
    abi_gate "${OUT}" "${READELF}"

    rm -f "${HERE}/aa-bridge-ilm.armhf" # a variant earlier builds made; not a deliverable

elif [ "${TARGET}" = "desktop" ]; then
    CXX="${CXX:-g++}"
    CXXFLAGS="-std=gnu++11 -O2 -g"
    DEFS='-DAA_DEFAULT_DECODER="avdec_h264" -DAA_DEFAULT_SINK="fakesink" -DAA_DEFAULT_AUDIO_SINK="fakesink" -DAA_DEFAULT_MIC_SRC="none"'
    INCLUDES="-I${HERE}/src"
    GST_CFLAGS="$(pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 2>/dev/null || true)"
    GST_LIBS="$(pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 2>/dev/null || echo '-lgstreamer-1.0 -lgstapp-1.0')"

    echo "==> desktop build: aa-bridge, feed_sender, feedproto_test"
    # shellcheck disable=SC2086
    "${CXX}" ${CXXFLAGS} ${DEFS} ${INCLUDES} ${GST_CFLAGS} "${SRCS[@]}" ${GST_LIBS} -lpthread -ldl \
        -o "${HERE}/aa-bridge"
    # shellcheck disable=SC2086
    "${CXX}" ${CXXFLAGS} ${INCLUDES} "${HERE}/tools/feed_sender.cpp" "${HERE}/src/feedproto.cpp" \
        -o "${HERE}/feed_sender"
    # shellcheck disable=SC2086
    "${CXX}" ${CXXFLAGS} ${INCLUDES} "${HERE}/tools/feedproto_test.cpp" "${HERE}/src/feedproto.cpp" \
        -o "${HERE}/feedproto_test"
    echo "built: ${HERE}/aa-bridge ${HERE}/feed_sender ${HERE}/feedproto_test"

else
    echo "usage: $0 [device|desktop]" >&2
    exit 2
fi
