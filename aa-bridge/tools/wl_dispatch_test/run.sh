#!/usr/bin/env bash
# Reproduces the unit's first-frame deadlock against a real (headless) weston and checks that
# the bridge's interposed wl_display_dispatch breaks it. Run from the work tree:
#   docker run --rm -v "$PWD":/w debian:bookworm bash /w/aa-bridge/tools/wl_dispatch_test/run.sh
set -u
apt-get update -qq >/dev/null && apt-get install -y -qq weston libwayland-dev g++ >/dev/null 2>&1 || { echo "apt failed"; exit 1; }
T=$(mktemp -d); cd "$T"
cp /w/aa-bridge/tools/wl_dispatch_test/{sink.c,main.cpp} /w/aa-bridge/src/wl_dispatch.cpp .
gcc -shared -fPIC -o libsink.so sink.c -lwayland-client || exit 1
g++ -o with main.cpp wl_dispatch.cpp -Wl,--dynamic-list=/w/aa-bridge/src/device_exports.list -ldl || exit 1
g++ -o without main.cpp -ldl || exit 1
export XDG_RUNTIME_DIR="$T/run" WAYLAND_DISPLAY=wayland-test
mkdir -m 700 -p "$XDG_RUNTIME_DIR"
weston --backend=headless-backend.so --socket=wayland-test --no-config > weston.log 2>&1 &
for _ in $(seq 1 50); do [ -S "$XDG_RUNTIME_DIR/wayland-test" ] && break; sleep 0.1; done
[ -S "$XDG_RUNTIME_DIR/wayland-test" ] || { echo "weston did not start"; cat weston.log; exit 1; }
fails=0
echo "== control (real wl_display_dispatch): must block, as on the unit"
./without | tee out1; grep -q 'RESULT blocked' out1 && echo "PASS: reproduces the deadlock" || { echo "FAIL"; fails=$((fails+1)); }
echo "== bridge (interposed wl_display_dispatch, 50 ms limit): must return"
./with 2>&1 | tee out2
grep -q 'interposed: active' out2 && grep -qE 'RESULT returned after ([0-9]|[0-9][0-9]|1[0-9][0-9]) ms' out2 \
    && echo "PASS: returns within the limit, interposer bound ahead of libwayland" || { echo "FAIL"; fails=$((fails+1)); }
[ "$fails" -eq 0 ] && echo "ALL PASSED" || echo "$fails FAILED"
exit "$fails"
