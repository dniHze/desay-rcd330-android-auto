# aa-carlife-shim

The first implementation of our CarLife service, in C++. It stands in for the unit's
`/svp/bin/carlifeservice`: it serves the stock CarLife RCF contract, so the unchanged AppConnect
and connection service drive Android Auto through the CarLife slot, and it drives the receiver
over a control socket.

**Public Beta 1 does not run it.** The image runs the Rust port, `aa-headless shim`, from
[`livi/native/crates/livi-carlife-shim`](../livi/native/crates/livi-carlife-shim), inside the
receiver's binary. The port follows this code class by class: the same state machines, timings,
`AA_*` settings, files, log lines and control protocol, over
[`rcf-lite`](../livi/native/crates/rcf-lite) instead of RCF 2.1 and boost. This folder stays for
two reasons:

- **The reference.** `src/appc_patch.hpp` is still the source of AppConnect's code changes: the
  image build bakes them into AppConnect from it (`kzb-re/patch_appconnect.py`), and the Rust
  shim's tests check its own tables against it. `tools/stock_ids.txt` holds the stock RCF method
  IDs both shims are checked against.
- **The test fakes.** Its build makes fake stock services, and `test_desktop.sh` runs either
  shim against them.

How the shim works: [`docs/architecture.md`](../docs/architecture.md#the-shim), and the RCF wire
protocol in [`docs/rcf.md`](../docs/rcf.md).

## Layout

| Path | What |
|---|---|
| `src/shim.cpp` | the shim |
| `src/carlife_idl.hpp` | the CarLife RCF interfaces and enums: the wire contract, in stock order |
| `src/*_idl.hpp` | the unit's other services the shim uses: audio, Bluetooth, CAN, iLM, location, power, remote, source |
| `src/appc_patch.hpp` | AppConnect's code changes (outgoing calls, iPhone, seek keys, phone key), with guard words per AppConnect build |
| `src/cl_probe.cpp` | an RCF client for the CarLife slot: getters, the listener, and opt-in write calls |
| `tools/fake_*.cpp` | stand-ins for the receiver, iLM, source, CAN, Bluetooth and power services, and syslogd |
| `tools/idcheck.cpp`, `tools/rcf_ids.py`, `tools/stock_ids.txt` | the method-ID check against the stock binaries |
| `tools/appc_patch_test.cpp` | a native test of `appc_patch.hpp` on a live process |
| `tools/bmp_edges.py`, `tools/trace2log.py` | helpers: padding in a screen dump; an strace capture turned into a log |
| `build.sh` | cross-build, ABI gate, method-ID check |
| `test_desktop.sh` | end-to-end test of the armhf binaries |

## Building

It needs the cross toolchain, the sysroot (for the unit's loader), the RCF 2.1.0.0 source in
`.scratch/rcf-dl/src/RCF-2.1.0.0` and the boost 1.56 headers in `.scratch/boost-dl/boost_1_56_0`.
Getting them: [`docs/building.md`](../docs/building.md#rcf-and-boost-only-for-the-c-shim).

```sh
docker run --rm --platform linux/arm64 -v "$PWD":/w desay-aa:latest bash /w/aa-carlife-shim/build.sh
```

It writes `out/aa-carlife-shim.armhf`, `out/cl_probe.armhf` and the fakes, each also unstripped.
The build:
- compiles RCF in from source, with one change in a copy: its timeouts read `CLOCK_MONOTONIC`
  instead of the wall clock, which the unit steps early in every boot;
- links libstdc++ statically, so nothing needs `GLIBCXX`, and fails unless each binary passes the
  ABI gate: the unit's loader as interpreter and last `NEEDED` entry, glibc 2.15 at most;
- reads the RCF method IDs back out of compiled client stubs and diffs them against
  `tools/stock_ids.txt`, read the same way from the stock `libcarlife.so` and `carlifeservice`.

The shim's build ID, in its start line and crash reports, is `src-` and the first 8 hex digits of
the md5 of its sources; `AA_VERSION` overrides it.

## Testing

`test_desktop.sh` starts the shim with the fakes and checks it over real RCF on TCP: fake mode;
receiver mode over the control socket; a missing, silent, hung or replaced receiver; ports
taken and other start-up failures; a stale registry file; the real receiver against the shim;
`Show(n)` on the iLM service; the CarLife audio sources and audio focus; CAN data; the phone's
microphone; the unit's Bluetooth address; Bluetooth pairing, music and the phone-link lock; the
unit switched off and on, and the last mode; SIGTERM during a session; logging; a crash.

On Debian armhf:

```sh
docker run --rm --platform linux/arm/v7 -v "$PWD":/w debian:bookworm bash /w/aa-carlife-shim/test_desktop.sh
```

On the unit's own loader and glibc 2.15, from the sysroot:

```sh
docker run --rm --platform linux/arm64 -v "$PWD":/w debian:bookworm bash -c \
  'apt-get update -qq && apt-get install -y -qq qemu-user procps >/dev/null &&
   QEMU_SYSROOT=/w/.scratch/sysroot bash /w/aa-carlife-shim/test_desktop.sh'
```

Against the Rust shim, which Public Beta 1 runs: `SHIM=` names the program to test. The image's
own stub, `sysupdate/svp/bin/aa-carlife-shim`, runs `aa-headless shim` from its folder:

```sh
docker run --rm --platform linux/arm/v7 -v "$PWD":/w debian:bookworm bash -c '
  mkdir -p /tmp/rs && cp /w/sysupdate/svp/bin/aa-carlife-shim /tmp/rs/ && ln -s /w/aa-headless.armhf /tmp/rs/aa-headless &&
  SHIM=/tmp/rs/aa-carlife-shim bash /w/aa-carlife-shim/test_desktop.sh'
```

It needs `aa-headless.armhf` from `r2_musl.sh`, and the fakes from `build.sh`. Section 7 runs the
real receiver against either shim.

`appc_patch.hpp` on a native Linux host (not under qemu-user, whose guest memory is not at its
guest addresses in `/proc/<pid>/mem`):

```sh
docker run --rm -v "$PWD":/w rust:bookworm sh -c 'g++ -std=c++11 -Wall -O2 -o /tmp/t /w/aa-carlife-shim/tools/appc_patch_test.cpp && /tmp/t'
```

## cl_probe

`cl_probe` talks to the CarLife slot (`127.0.0.1:30407`, publisher `30408`), whichever service
holds it:

| Command | |
|---|---|
| `status`, `ro` | the connect status, or every getter |
| `listen [seconds]` | subscribe and print the callbacks |
| `connect`, `disconnect`, `show <n>`, `touch`, `rotate`, `play`, `pause`, … | calls that change state; only with `--allow-write` |

Only getters run by default. Do not use `--allow-write` against the stock service.

## License

**GPL-2.0-only** ([`LICENSES/GPL-2.0.txt`](../LICENSES/GPL-2.0.txt)). These programs compile in
RCF 2.1.0.0 by Delta V Software, whose open-source license is GPL v2. The rest of the repository
is GPL-3.0-or-later; the Rust shim, which links no RCF code, is GPL-3.0-or-later with the
receiver.
