#!/usr/bin/env bash
# Cross-build the receiver (livi-aa-headless) as a static armv7 musl binary and install a
# stripped copy at ./aa-headless.armhf. Run from the work dir:
#
#   docker run --rm --platform linux/arm64 -v "$PWD":/w -v "$PWD/xcache-musl":/cache \
#     messense/rust-musl-cross:armv7-musleabihf bash /w/r2_musl.sh
#
# LIVI is livi-src/ (a LIVI checkout, our receiver/patches as its last commits) or, in the public
# repository, livi/ (the crates the receiver needs with the patches applied; the upstream commit
# in livi/UPSTREAM).
#
# Unstripped binary (for gdb): <LIVI>/native/target/armv7-unknown-linux-musleabihf/release/livi-aa-headless
set -uo pipefail
LIVI=/w/livi-src
[ -d "$LIVI/native" ] || LIVI=/w/livi
export CARGO_HOME=/cache/cargo RUSTUP_HOME=/cache/rustup
export PATH=/cache/cargo/bin:/usr/local/musl/bin:$PATH
# Select the musl cross-linker explicitly: otherwise cargo's default `cc` driver lets the
# host (aarch64) /usr/bin/ld try to link armv7 musl crt objects -> "file in wrong format".
MUSL_GCC="$(command -v armv7-unknown-linux-musleabihf-gcc || echo armv7-unknown-linux-musleabihf-gcc)"
MUSL_STRIP="$(command -v armv7-unknown-linux-musleabihf-strip || echo armv7-unknown-linux-musleabihf-strip)"
export CARGO_TARGET_ARMV7_UNKNOWN_LINUX_MUSLEABIHF_LINKER="$MUSL_GCC"
export CC_armv7_unknown_linux_musleabihf="$MUSL_GCC"
# LIVI's head-unit certificate and key (livi-aa/src/cert.rs, unchanged by our patches) are not
# in the public repository: fetched from upstream at the commit in livi/UPSTREAM, and checked.
CERT="$LIVI/native/crates/livi-aa/src/cert.rs"
CERT_SHA256=8e4340ff289ff23837b5044ca02fe6899d2e04129f7681adbc97acb9743990b8
if [ ! -f "$CERT" ]; then
    U=$(head -n 1 "$LIVI/UPSTREAM")
    echo "[0] livi-aa/src/cert.rs from LIVI $U"
    curl -fsSL "https://raw.githubusercontent.com/f-io/LIVI/$U/native/crates/livi-aa/src/cert.rs" -o "$CERT.part" &&
        echo "$CERT_SHA256  $CERT.part" | sha256sum -c --quiet && mv "$CERT.part" "$CERT" ||
        { rm -f "$CERT.part"; echo "cannot fetch livi-aa/src/cert.rs"; exit 1; }
fi
cd "$LIVI/native"
echo "[0] musl gcc: $MUSL_GCC"
echo "[1] toolchain (pinned beta auto-installs)"; rustup show 2>&1 | tail -2
echo "[2] add musl target"; rustup target add armv7-unknown-linux-musleabihf 2>&1 | tail -2
echo "[3] build release (static musl, panic=abort)"
# panic=abort, only for this build (the release profile is otherwise the one that ran in the
# car; the workspace's `embedded` profile also changes opt-level and LTO). Under unwind a panic
# in a tokio task ends just that task and leaves a half-dead receiver procguard cannot see; a
# panic in the main task exits 101, and procguard gives up on a non-zero exit. With abort
# every panic is a SIGABRT, which procguard restarts within the launcher's crash budget.
CARGO_PROFILE_RELEASE_PANIC=abort \
  cargo build --release -p livi-aa-headless --target armv7-unknown-linux-musleabihf 2>&1 | tail -30
rc=${PIPESTATUS[0]}
echo "rc=$rc"
[ "$rc" -eq 0 ] || exit "$rc"
echo "[4] strip -> /w/aa-headless.armhf"
"$MUSL_STRIP" -o /w/aa-headless.armhf target/armv7-unknown-linux-musleabihf/release/livi-aa-headless
ls -l /w/aa-headless.armhf
# The Android Auto version Settings > System information shows, crediting upstream LIVI
# (https://github.com/f-io/LIVI): LIVI-<UTC build date>-<the LIVI commit our patches apply to>,
# followed by the binary's md5 (sysupdate/build_update.py ships it only with that binary).
if [ -f "$LIVI/UPSTREAM" ]; then
    BASE=$(head -c 7 "$LIVI/UPSTREAM")
else
    N=$(ls /w/receiver/patches/*.patch | wc -l)
    BASE=$(git -c safe.directory='*' -C "$LIVI" rev-parse --short=7 "HEAD~$N") || exit 1
fi
echo "LIVI-$(date -u +%F)-$BASE $(md5sum /w/aa-headless.armhf | cut -d' ' -f1)" > /w/aa-headless.armhf.version
echo "[5] $(cat /w/aa-headless.armhf.version)"
