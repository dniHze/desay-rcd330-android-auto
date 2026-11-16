# aa-bridge

The bridge between the receiver and the unit's media hardware, in C++. The receiver,
`aa-headless`, writes the phone's video and audio to a Unix socket (LIVI's feed framing,
`livi-session-io`); the bridge listens on it and:

- **video:** decodes the H.264 stream on the unit's IVA-HD decoder (`ducatih264dec`) through
  GStreamer 1.2.3, onto a `waylandsink2` surface that the CarLife shim shows. The shim asks the
  running bridge, over its crop socket (`AA_BRIDGE_CTL`), to crop that surface through the
  unit's `libilmControl`, which the static receiver cannot load;
- **audio:** plays media on the unit's ALSA output, and the speech, system and telephony streams
  as PulseAudio streams named as the stock CarLife service's, which the shim registers with the
  unit's audio manager for their volume;
- **microphone:** sends the car's microphone (the Bluetooth module's capture) back to the phone.

It stays C++ because it loads the unit's own GStreamer, glibc 2.15 libraries, which a static
musl binary cannot. How it fits in: [`docs/architecture.md`](../docs/architecture.md#the-bridge).

## Building

It needs the cross toolchain and the sysroot ([`docs/building.md`](../docs/building.md)).

```sh
docker run --rm --platform linux/arm64 -v "$PWD":/w desay-aa:latest bash /w/aa-bridge/build.sh device
```

It writes `aa-bridge.armhf` (stripped, for the image) and `aa-bridge.unstripped.armhf` (for gdb).

- **Headers:** the sysroot has GStreamer's libraries but no headers, so `src/gst_min.h` declares
  the GStreamer 1.0 calls the bridge uses, and it links against the unit's own
  `libgstreamer-1.0.so.0` and `libgstapp-1.0.so.0`. A symbol the unit lacks fails the link.
- **ABI gate:** the build fails unless the binary has the unit's loader as interpreter and last
  `NEEDED` entry, needs glibc 2.15 at most, and no `GLIBCXX` (libstdc++ is static).
- **Interposers:** the device build exports two functions the unit's plugins then bind to
  (`src/device_exports.list`): a time-limited `wl_display_dispatch` for its Wayland sink
  (`src/wl_dispatch.cpp`), and `VIDDEC3_create` with the H.264 display delay of the unit's own
  player (`src/dce_params.cpp`).

`build.sh desktop` builds a native variant with a software decoder (`avdec_h264`) and
`fakesink`, plus `feed_sender` and `feedproto_test`, for the smoke test.

Off the unit, `--decoder identity` passes the H.264 stream on undecoded, as in the emulator
(`AA_VIDEO_DECODER` in `aa.conf`); `aa-bridge --help` lists every option.

## Tests

Run each from the repository root.

| Test | Command | What it checks |
|---|---|---|
| Smoke test | `docker run --rm --platform linux/arm64 -v "$PWD":/work debian:bookworm bash /work/aa-bridge/smoke_test.sh` | builds the desktop variant, runs `feedproto_test`, then streams a generated H.264 file through the bridge with `feed_sender` and checks that frames were parsed with no pipeline error |
| The unit's parser | `docker run --rm --platform linux/arm64 -v "$PWD":/w debian:bookworm bash /w/aa-bridge/tools/parser_latency_unit.sh` | the device build on the unit's GStreamer under QEMU: each frame leaves the unit's `h264parse` at once with the access unit delimiter the bridge appends, the drain, and a video error mid-session |
| Decoder delay | `docker run --rm -v "$PWD":/w debian:bookworm bash /w/aa-bridge/tools/dce_params_test/run.sh` | the bridge's `VIDDEC3_create` binds ahead of `libdce`'s for a plugin opened `RTLD_LOCAL`, and sets the display delay |
| Wayland dispatch | `docker run --rm -v "$PWD":/w debian:bookworm bash /w/aa-bridge/tools/wl_dispatch_test/run.sh` | reproduces the first-frame deadlock against a headless weston, and checks that the bridge's `wl_display_dispatch` breaks it |
| Resampler | `docker run --rm -v "$PWD":/w debian:bookworm bash /w/aa-bridge/tools/resample_test/run.sh` | the 16 to 48 kHz upsampler (`src/resample.h`) |

`feedproto_test` ports the Rust tests of LIVI's feed framing, so the C++ reader is checked
against the Rust writer byte for byte. `parser_latency_unit.sh` needs `aa-bridge.armhf` and the
sysroot.

## License

GPL-3.0-or-later, as the rest of the repository.
