# livi

The part of [LIVI](https://github.com/f-io/LIVI) that the receiver, `aa-headless`, is built
from, with our crates and our changes. LIVI is Lasse Heitgres's open-source in-vehicle
infotainment stack; this project uses its Android Auto implementation.

| | |
|---|---|
| Upstream | https://github.com/f-io/LIVI |
| Commit | `daf905656ac4df88136c28fa2171e9cc28ee46a9` (2026-11-06), recorded in `UPSTREAM` |
| Our changes to LIVI's crates | `upstream.diff` |
| Rust toolchain | pinned by `rust-toolchain.toml` |
| License | GPL-3.0-or-later (`LICENSE`), with the attribution term in `NOTICE` |

## Crates

`native/Cargo.toml` is LIVI's workspace, trimmed to these crates. `aa-headless` depends on all of
them, and on nothing else of LIVI's.

| Crate | From | What |
|---|---|---|
| `livi-aa-headless` | ours, added | The headless Android Auto receiver: the phone over USB (accessory mode) or over TCP (`AA_TCP_CONNECT`), video and audio to the bridge's feed socket, the microphone, the control socket to the shim, the steering-wheel keys. `aa-headless shim` runs the CarLife shim in the same binary instead. |
| `livi-carlife-shim` | ours, added | The CarLife service's stand-in, a port of [`aa-carlife-shim`](../aa-carlife-shim): the stock CarLife RCF contract for AppConnect, and the unit's services (iLM, audio sources, CAN, Bluetooth, power) behind it |
| `rcf-lite` | ours, added | RCF 2.x (runtime version 12) over TCP, wire-compatible with the unit's services: client, server, publish and subscribe ([`docs/rcf.md`](../docs/rcf.md)) |
| `livi-aa` | LIVI | Android Auto sessions, the USB watcher and accessory-mode switch, the head-unit certificate |
| `livi-aa-stack` | LIVI | The Android Auto protocol: discovery, channels, control, input, sensors |
| `livi-aa-proto` | LIVI | Android Auto's protocol constants |
| `livi-core-proto` | LIVI | LIVI's core message types: Android Auto events, input, GNSS, the helper protocol |
| `livi-host-proto` | LIVI | The host side's types: feed records, control, display |
| `livi-session-io` | LIVI | Session I/O: the feed socket's writer and framing, sockets, USB links |
| `livi-wifi` | LIVI | Wi-Fi access point types for wireless Android Auto, which `livi-aa` and `livi-aa-stack` depend on |

Our three crates are whole here and are not in `upstream.diff`.

## Our changes to LIVI's crates

`upstream.diff` is one cumulative diff, from the upstream commit to Public Beta 1, of LIVI's own
crates and the workspace `Cargo.toml`. What it changes:

- **Bluetooth pairing through the car:** the phone's pairing request is answered by the host,
  which asks the car's own Bluetooth.
- **Focus:** audio and video focus sent unsolicited, so that the phone stops projecting while
  the car hides Android Auto (video focus NATIVE), and its autoplay can be refused once (the last
  mode).
- **Sensors:** the advertised sensors are configurable, defaulting to the driving set.
- **Gating:** the USB watcher switches and serves a phone only once the host arms it (the shim's
  `start`), and arming takes up phones already attached. A TCP session follows the same gate.
- **The feed:** reopened after the bridge restarts, with the stream announced again and a
  keyframe requested; record limits; a stop bound to its own session.
- **Keys:** the host can send a key down and up, with Android Auto's long-press flag, for the held
  seek keys.
- **Logging:** the phone's first three ping timestamps per session; the navigation state only
  when it changes, its position at debug level.
- **The workspace:** trimmed to these crates, with ours added.

The changes to `livi-aa-headless` (the seek modes, `AA_TCP_CONNECT`, the shim entry) are in that
crate's own code.

## The certificate

LIVI bundles a head-unit certificate and key in `native/crates/livi-aa/src/cert.rs`. That file
is not in this repository, and our changes do not touch it. `r2_musl.sh` downloads it from
upstream LIVI at the commit in `UPSTREAM` when it is missing, and checks its sha256;
`.gitignore` keeps it out of commits. Fetching it by hand, for `cargo test`:
[`docs/building.md`](../docs/building.md#verifying).

## Building

From the repository root:

```sh
docker run --rm --platform linux/arm64 -v "$PWD":/w -v "$PWD/xcache-musl":/cache messense/rust-musl-cross:armv7-musleabihf bash /w/r2_musl.sh
```

It builds `livi-aa-headless` as a static armv7 musl binary (with `panic=abort`) and writes
`aa-headless.armhf` and `aa-headless.armhf.version` at the repository root. The version stamp is
`LIVI-<UTC build date>-<the first 7 characters of UPSTREAM> <md5>`; the unit shows its first part
in Settings > System information. Tests and the rest of the build:
[`docs/building.md`](../docs/building.md).

## License

LIVI is free software under the GNU General Public License v3.0 or later (`LICENSE`), and so is
everything in this folder, our crates included. `NOTICE` adds a term under section 7(b): any work
that contains LIVI or a part of it must keep this attribution, in its legal notices and in its
documentation:

> Based on LIVI by Lasse Heitgres (https://github.com/f-io/LIVI)

`CREDITS.md` is LIVI's own list of credits and third-party components.
