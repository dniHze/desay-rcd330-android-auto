#!/usr/bin/env bash
# Checks that the bridge's VIDDEC3_create (src/dce_params.cpp) binds ahead of libdce's for a
# plugin opened RTLD_LOCAL, as gst-ducati is, and sets the H.264 display delay. Run from the
# work tree:
#   docker run --rm -v "$PWD":/w debian:bookworm bash /w/aa-bridge/tools/dce_params_test/run.sh
set -u
command -v g++ > /dev/null || { apt-get update -qq > /dev/null && apt-get install -y -qq g++ > /dev/null 2>&1; } || { echo "apt failed"; exit 1; }
T=$(mktemp -d); cd "$T"
cp /w/aa-bridge/tools/dce_params_test/{dce.c,plugin.c,main.cpp} /w/aa-bridge/src/dce_params.cpp .
gcc -shared -fPIC -Wl,-soname,libdce.so.1 -o libdce.so.1 dce.c || exit 1
gcc -shared -fPIC -o libplugin.so plugin.c -L. -l:libdce.so.1 || exit 1
g++ -o with main.cpp dce_params.cpp -Wl,--dynamic-list=/w/aa-bridge/src/device_exports.list -ldl || exit 1
g++ -o without main.cpp -ldl || exit 1
export LD_LIBRARY_PATH="$T"
fails=0
check() { # $1 = name, $2 = output file, $3 = pattern
    if grep -q "$3" "$2"; then echo "PASS: $1"; else echo "FAIL: $1"; fails=$((fails+1)); fi
}
echo "== control (no interposer): the plugin's -1 reaches libdce"
./without | tee out0
check "control passes AUTO through" out0 'libdce: VIDDEC3_create(ivahd_h264dec) displayDelay -1'
echo "== bridge (interposed VIDDEC3_create)"
./with 2>&1 | tee out1
check "H.264 decoder created in decode order" out1 'libdce: VIDDEC3_create(ivahd_h264dec) displayDelay 0'
check "the change is logged" out1 'VIDDEC3_create(ivahd_h264dec): displayDelay -1 -> 0'
check "other codecs left alone" out1 'libdce: VIDDEC3_create(ivahd_mpeg4dec) displayDelay -1'
echo "== AA_VIDEO_DISPLAY_DELAY=auto"
AA_VIDEO_DISPLAY_DELAY=auto ./with 2>&1 | tee out2
check "auto leaves the plugin's value" out2 'libdce: VIDDEC3_create(ivahd_h264dec) displayDelay -1'
echo "== AA_VIDEO_DISPLAY_DELAY=1"
AA_VIDEO_DISPLAY_DELAY=1 ./with 2>&1 | tee out3
check "an explicit value is used" out3 'libdce: VIDDEC3_create(ivahd_h264dec) displayDelay 1'
[ "$fails" -eq 0 ] && echo "ALL PASSED" || echo "$fails FAILED"
exit "$fails"
