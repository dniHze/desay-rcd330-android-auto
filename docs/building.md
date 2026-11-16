# Building from source

This page builds every part of the system update from this repository and your own stock
image, and runs the tests. Run every command from the repository root.

| Output | Built by | Where it lands |
|---|---|---|
| Receiver and CarLife shim, `aa-headless.armhf` (+ `.version`) | `r2_musl.sh` | repository root |
| Bridge, `aa-bridge.armhf` | `aa-bridge/build.sh` | `aa-bridge/` |
| UI library, language files, AppConnect rebrand | `sysupdate/build_update.py` (runs `i18n/build.py` and `kzb-re/`) | inside the image build |
| The system update image | `sysupdate/build_update.py` | `out/<build id>/` |
| C++ shim, `cl_probe` and the fake services (tests only) | `aa-carlife-shim/build.sh` | `aa-carlife-shim/out/` |

How the parts fit together: [architecture.md](architecture.md).

## Prerequisites

- **Docker.** Every build runs in a `linux/arm64` container, and the shim tests also in a
  `linux/arm/v7` one. An arm64 host (Apple silicon, an arm64 Linux machine) runs them natively.
  An x86-64 host needs QEMU user emulation registered with Docker (Docker Desktop has it), and
  is much slower.
- **Python 3.12 or later**, with **Pillow** installed for that interpreter. The scripts run with
  `python3 -I`, which ignores Python's user site directory, so a `pip install --user` Pillow is not seen.
- **git**, **curl**, `shasum`, and network access for the first builds (Rust crates and toolchain,
  crosstool-NG sources, Debian packages).

### What you supply

Nothing of VW's is in this repository. These come from you, and never go into it:

| Input | Where it goes | Needed for |
|---|---|---|
| Stock update image of the 0317 base, with its `.md5` | `firmware/PQ_6RD_035_187B/SW0317_20200303/6RD_035_187B_TR7247_SW0317_20200303.bin` | the sysroot, the language sheet, an 0317 image |
| Stock update image of the 0394 base, with its `.md5` | `firmware/MQB_5GG_035_280D/SW0394_20200303/5GG_035_280D_TR8257_SW0394_20200303.bin` | the language sheet, an 0394 image |
| Cross toolchain | `.scratch/x-tools/` | built by `ctng/` (below) |
| The unit's sysroot | `.scratch/sysroot/` | made from a stock image (below) |
| RCF 2.1.0.0 source and boost 1.56 headers | `.scratch/rcf-dl/`, `.scratch/boost-dl/` | the C++ shim only (below) |

The bases are 0317, PQ 6RD 035 187B (SW0317), and 0394, MQB 5GG 035 280D (SW0394). The image
build and the language tools check each image's md5 against the stock ones pinned in
`sysupdate/build_update.py` (`BASES`) and refuse anything else; the language tools read the
images from these folders. `i18n/extract.py` and `i18n/test_lib.py` read **both** images, so an
image build needs both even when it is for one base.

## One-time setup

### The build image

```sh
docker build --platform linux/arm64 -t desay-aa toolchain
```

`desay-aa:latest` (Debian bookworm) holds the cross binutils, `debugfs`, `objdump` and Python
that the scripts below call.

### The cross toolchain

crosstool-NG builds gcc 6.5 with glibc 2.12.1, binutils 2.26.1 and Linux 3.16 headers for the
unit's Cortex-A15 (hard float), `arm-cortex_a15-linux-gnueabihf`, from `ctng/desay.config`. A
binary linked with it needs nothing newer than the unit's glibc 2.15.

```sh
docker build --platform linux/arm64 -t ctng -f toolchain/Dockerfile.crosstool-ng toolchain
mkdir -p ctng/tarballs && chmod a+w ctng/tarballs
docker run --rm --platform linux/arm64 -v "$PWD":/work ctng bash /work/ctng/build_toolchain.sh
mkdir -p .scratch && tar -C .scratch -xf x-tools.tar
```

The sources are downloaded into `ctng/tarballs/`. The result is
`.scratch/x-tools/arm-cortex_a15-linux-gnueabihf/`; its programs are arm64 Linux binaries, so they
run inside `desay-aa:latest`.

### The sysroot

The unit's root file system with its `/svp` tree, from a stock image. The bridge and the UI
library link against its libraries, and the tests run on its loader and glibc under QEMU.
`os.bin` is the same in both bases, so either image will do.

```sh
python3 -I tools/desay_pkg.py unpack firmware/PQ_6RD_035_187B/SW0317_20200303/6RD_035_187B_TR7247_SW0317_20200303.bin .scratch/fw
python3 -I - <<'EOF'
import re, zlib
d = open(".scratch/fw/os.bin", "rb").read()
at = re.search(rb"\x1f\x8b\x08\x08.{6}targetfs\.bin\x00", d, re.S).start()
open(".scratch/fw/targetfs.img", "wb").write(zlib.decompressobj(31).decompress(d[at:]))
EOF
docker run --rm --platform linux/arm64 -v "$PWD":/w desay-aa:latest bash -c '
  mkdir -p /w/.scratch/sysroot/svp &&
  debugfs -R "rdump / /w/.scratch/sysroot" /w/.scratch/fw/targetfs.img &&
  tar -xzf /w/.scratch/fw/svp.bin -C /w/.scratch/sysroot/svp &&
  python3 /w/toolchain/mk_sysroot.py /w/.scratch/sysroot'
```

`targetfs.bin`, inside `os.bin`, is the gzip'd ext4 image of the root file system. On a
case-insensitive file system (macOS) `debugfs` reports a few `terminfo` folders as existing; that
is harmless. `toolchain/mk_sysroot.py` turns `Link: <target>` placeholder files, which some
extractors write instead of symlinks, back into symlinks; after `debugfs` it finds none.

### RCF and boost (only for the C++ shim)

The C++ shim compiles in RCF 2.1.0.0, the open-source release of Delta V Software's RPC library
(GPL v2), with boost 1.56 headers. `deltavsoft.com` no longer serves it; the Internet Archive has
the original file.

```sh
mkdir -p .scratch/rcf-dl/src .scratch/boost-dl
curl -fL -o .scratch/rcf-dl/RCF-2.1.0.0.tar.gz "https://web.archive.org/web/20161013041203id_/http://www.deltavsoft.com/downloads/RCF-2.1.0.0.tar.gz"
curl -fL -o .scratch/boost-dl/boost_1_56_0.tar.bz2 https://archives.boost.io/release/1.56.0/source/boost_1_56_0.tar.bz2
shasum -a 256 -c <<'EOF'
0c09b4ad88e7a21e895b7d222be592b6622925e83543d9e37b90c77d211be41f  .scratch/rcf-dl/RCF-2.1.0.0.tar.gz
134732acaf3a6e7eba85988118d943f0fa6b7f0850f65131fff89823ad30ff1d  .scratch/boost-dl/boost_1_56_0.tar.bz2
EOF
tar -C .scratch/rcf-dl/src -xzf .scratch/rcf-dl/RCF-2.1.0.0.tar.gz
tar -C .scratch/boost-dl -xjf .scratch/boost-dl/boost_1_56_0.tar.bz2 boost_1_56_0/boost boost_1_56_0/LICENSE_1_0.txt
```

`aa-carlife-shim/build.sh` expects `.scratch/rcf-dl/src/RCF-2.1.0.0` and
`.scratch/boost-dl/boost_1_56_0` (or `RCF_DIR` and `BOOST_DIR`).

## The receiver

`aa-headless` is LIVI's Android Auto stack as a headless receiver, with the CarLife shim inside
(`aa-headless shim`). It is a static armv7 musl binary built from [`livi/`](../livi).

```sh
docker run --rm --platform linux/arm64 -v "$PWD":/w -v "$PWD/xcache-musl":/cache messense/rust-musl-cross:armv7-musleabihf bash /w/r2_musl.sh
```

- The first run installs the Rust toolchain pinned by `livi/rust-toolchain.toml` and the crates
  into `xcache-musl/`, which later runs reuse.
- LIVI's head-unit certificate, `livi/native/crates/livi-aa/src/cert.rs`, is not in this
  repository. `r2_musl.sh` downloads it from upstream LIVI at the commit in `livi/UPSTREAM` and
  checks its sha256.
- It writes `aa-headless.armhf` (stripped) and `aa-headless.armhf.version`:
  `LIVI-<UTC build date>-<LIVI commit> <md5 of aa-headless.armhf>`. The image build ships the
  binary only with a stamp that names its md5, and shows the stamp's first part in
  Settings > System information.
- The unstripped binary, for gdb, stays in
  `livi/native/target/armv7-unknown-linux-musleabihf/release/livi-aa-headless`.

## The bridge

```sh
docker run --rm --platform linux/arm64 -v "$PWD":/w desay-aa:latest bash /w/aa-bridge/build.sh device
```

It links against the unit's GStreamer 1.2.3 from the sysroot and writes
`aa-bridge/aa-bridge.armhf`, plus `aa-bridge.unstripped.armhf` with symbols. The build fails
unless the binary passes the ABI gate: the unit's loader, glibc 2.15 at most, no `GLIBCXX`.

## The C++ shim and the fakes (optional)

Public Beta 1 runs the Rust shim inside `aa-headless`. The C++ shim is the reference it was
ported from, and its build makes the fake stock services the shim tests run against.

```sh
docker run --rm --platform linux/arm64 -v "$PWD":/w desay-aa:latest bash /w/aa-carlife-shim/build.sh
```

Output in `aa-carlife-shim/out/`. See [`aa-carlife-shim/README.md`](../aa-carlife-shim/README.md).

## The UI languages

The translation sheet, `i18n/strings.csv`, ships without the stock English and Chinese texts of
the phrase-book rows (ids 0–1652), as those are VW's. Fill them from your images once, before the
first image build:

```sh
python3 -I i18n/extract.py
python3 -I i18n/check.py
```

`extract.py` unpacks both images into `out/i18n/` and disassembles the apps in `desay-aa:latest`.
The image build refuses a sheet whose English differs from the image's library. Keep the filled
`en` and `zh` cells of the phrase-book rows out of commits. The library itself is built by the
image build; details in [`i18n/README.md`](../i18n/README.md).

## A system update image

```sh
python3 -I sysupdate/build_update.py --base 0317 --build-id my-0317-1 --accept-new
python3 -I sysupdate/build_update.py --base 0394 --build-id my-0394-1 --accept-new
```

It needs `aa-headless.armhf` with its stamp, `aa-bridge/aa-bridge.armhf`, the toolchain, the
sysroot, the filled sheet and the base's stock image. `--label "<name>"` names a release: the
release notes, `aa_version` and Settings > System information then show `<name> (LIVI)`.

**The lock.** An image ships only artifacts whose md5 is in `sysupdate/RELEASE.lock`. This
repository's lock lists the Public Beta 1 artifacts. A build from source differs from them
(paths, toolchain, build date), so build with `--accept-new`: it records your md5s in the lock,
and `RELEASE.md` says the image contains binaries "not yet run in the car". A second build from
the same inputs then passes without the flag.

The output is `out/<build id>/`: the image under the stock file name, its `.md5`, `RELEASE.md`
(the members added or replaced, with md5s) and `svp-tree/` (the staged `/svp` files). Steps and
checks: [`sysupdate/README.md`](../sysupdate/README.md).

## Verifying

| Suite | Command | What it covers |
|---|---|---|
| Launcher | `docker run --rm --privileged --platform linux/arm64 -v "$PWD":/w debian:bookworm bash /w/sysupdate/test_sysupdate.sh` | `aa_run.sh` and the stubs under busybox with a fake procguard: modes, supervision, budgets, boot-loop guard, card switches, rebrand guards, configuration, logs |
| Receiver crates | below | the receiver, the stack, the Rust shim (method IDs, AppConnect tables, usage, the patcher on a live process) and `rcf-lite` |
| Shim, end to end | `docker run --rm --platform linux/arm/v7 -v "$PWD":/w debian:bookworm bash /w/aa-carlife-shim/test_desktop.sh` | the shim against fake stock services over real RCF; also on the unit's loader and against the Rust shim ([`aa-carlife-shim/README.md`](../aa-carlife-shim/README.md)) |
| Bridge | `docker run --rm --platform linux/arm64 -v "$PWD":/work debian:bookworm bash /work/aa-bridge/smoke_test.sh` | feed framing and the GStreamer pipeline with a software decoder; more in [`aa-bridge/README.md`](../aa-bridge/README.md) |
| UI library | `python3 -I i18n/test_lib.py` | our `libUIPublic.so` on the unit's loader under QEMU, both bases and the Android Auto variant |
| Sheet | `python3 -I i18n/check.py` | placeholders, protected terms, line breaks, the unit's font |

The receiver crates' tests run on the host's architecture, in the receiver's build container.
Build the receiver once first: `r2_musl.sh` installs the pinned toolchain into `xcache-musl/`
and fetches `cert.rs`. Then:

```sh
docker run --rm --platform linux/arm64 -v "$PWD":/w -v "$PWD/xcache-musl":/cache \
  messense/rust-musl-cross:armv7-musleabihf bash -c \
  'export CARGO_HOME=/cache/cargo RUSTUP_HOME=/cache/rustup PATH=/cache/cargo/bin:$PATH
   cd /w/livi/native && cargo test --target aarch64-unknown-linux-gnu \
     -p livi-aa-headless -p livi-carlife-shim -p rcf-lite -p livi-aa-stack -p livi-aa -p livi-session-io'
```

The container builds for the unit by default; `--target` runs the tests on the container's own
architecture.

### Testing without a car

The [RCD330 Plus Desay Emulator](https://github.com/Alonia123/RCD330-Plus-Desay-Emulator) by
Alonia123 runs the unit's software from a stock image under QEMU, with its screen and touch. Our
`/svp` files (`out/<build id>/svp-tree/`), the UI library and the whole Android Auto stack run in
it. The emulator has no USB phone and no hardware video decoder, so three keys in the
emulated unit's `/storage/data/data/aa/aa.conf` route around them:

| Key | Effect |
|---|---|
| `AA_TCP_CONNECT=<host>:5277` | The receiver connects to a phone's head unit server (Android Auto's developer setting "Start head unit server", reached through `adb forward tcp:5277 tcp:5277`) instead of USB |
| `AA_VIDEO_DECODER=identity` | The bridge passes the H.264 stream on undecoded |
| `AA_VIDEO_SINK=tcpserversink` | The bridge serves that stream over TCP instead of showing it |

`cl_probe --allow-write connect`, then `cl_probe --allow-write show 1`, from the C++ shim's
build, stand in for the unit's connection service and AppConnect: they start a session and show
it.

## Flashing

Flashing, the switches and the way back to stock:
[releases/public-beta-1.md](releases/public-beta-1.md).
