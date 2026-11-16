# System update

This folder builds the system update that puts Android Auto into the unit's system, and holds
the launcher that starts it on the unit.

| File | What it is |
|---|---|
| `build_update.py` | Builds the update from a stock image and verifies it from its bytes |
| `RELEASE.lock` | The md5 of every artifact an image may ship |
| `svp/bin/carlifeservice` | The CarLife slot's new entry point: a stub that runs the launcher |
| `svp/bin/aa-carlife-shim` | A stub that execs `aa-headless shim`, our CarLife service |
| `svp/etc/aa_run.sh` | The launcher: mode, rebrand, supervision, fallbacks, logs |
| `svp/etc/aa.conf` | The defaults; `/storage/data/data/aa/aa.conf` on the unit overrides them |
| `test_sysupdate.sh` | The launcher's tests, under busybox with a fake procguard |

## What the image changes

Only `svp.bin`, the `/svp` partition, differs from stock. `app-pkg.bin`, `radio.bin` and
`os.bin` are byte-identical, and the image keeps the stock VERSION, ROLL_BACK_NO (1/1/1/5) and
PROJECT lines, so the stock image can always be flashed back. The container has no signature,
only md5s. `init.sh` is not changed: the worst our files can do is affect the CarLife slot.

| `svp.bin` member | |
|---|---|
| `bin/carlifeservice` | replaced: the stub |
| `bin/carlifeservice.stock` | added: the stock CarLife service, byte-identical, the fallback |
| `bin/aa-carlife-shim`, `bin/aa-headless` | added: the shim's stub; the receiver, which is also the shim |
| `bin/aa-bridge` | added: video, audio and microphone |
| `etc/aa_run.sh`, `etc/aa.conf` | added: the launcher and its defaults |
| `etc/aa_rebrand.md5`, `etc/aa_version`, `etc/aa_android_auto_version` | added, generated: the rebrand's guards; the build ID and member md5s; the version Settings shows |
| `share/aa_appconnect`, `share/aa_VW_MP2017.kzb`, `share/aa_libUIPublic.so` | added: AppConnect with Android Auto in place of CarLife, its images, the UI library's Android Auto variant |
| `lib/libUIPublic.so` | replaced: our UI library, with the languages and Settings > Appearance ([`i18n/`](../i18n)) |
| `share/aa_lang.bin`, `share/aa_lang_<flag>.png`, `share/aa_theme_<name>.png`, `share/aa_blank.png` | added: the languages' texts, the flags, Appearance's pictures |

The rebranded AppConnect files are bind-mounted over the stock ones at boot, before AppConnect
starts, and only while each target is the stock file it was made from. The app package stays
stock, so with Android Auto off AppConnect is fully stock again.

## Building

```sh
python3 -I sysupdate/build_update.py --base 0317 --build-id my-0317-1 --accept-new
```

| Option | |
|---|---|
| `--base 0317` | PQ 6RD 035 187B (SW0317), the default |
| `--base 0394` | MQB 5GG 035 280D (SW0394) |
| `--build-id <id>` | names `out/<id>/`; default `aa-<YYYYMMDD>` |
| `--label "<name>"` | a release's name, in `RELEASE.md`, `aa_version` and Settings > System information (`<name> (LIVI)`) |
| `--accept-new` | record the current artifacts' md5s in the lock instead of refusing them |
| `--stock-dir <dir>` | the stock image's folder, instead of the base's usual one under `firmware/` |

**Inputs**, all checked:
- the base's stock image and its `.md5`, at `firmware/PQ_6RD_035_187B/SW0317_20200303/` or
  `firmware/MQB_5GG_035_280D/SW0394_20200303/`, pinned by md5 in `BASES`;
- `aa-headless.armhf` and `aa-headless.armhf.version` from `r2_musl.sh`, whose stamp must name
  the receiver's md5;
- `aa-bridge/aa-bridge.armhf`;
- the files in `svp/`, and the translation sheet `i18n/strings.csv` (with the stock texts filled
  in by `i18n/extract.py`);
- Docker with `desay-aa:latest`, the cross toolchain and the sysroot, for the UI library.

Setting all of this up: [`docs/building.md`](../docs/building.md).

**What it does:**
1. Unpacks the stock container and checks every component's md5.
2. Takes AppConnect, its resources, `libUIPublic.so`, `carlifeservice`, the Settings app and the
   launcher app out of the image itself, rebuilds the rebrand from them with `kzb-re/` (texts,
   AppConnect's code changes from `aa-carlife-shim/src/appc_patch.hpp`, images), and builds the
   UI library and language files with `i18n/build.py`.
3. Checks every artifact against `RELEASE.lock`, and the receiver's stamp against its md5.
4. Stages the new `/svp` files and writes `aa_version` and `aa_rebrand.md5`.
5. Rebuilds `svp.bin` from the stock one with `tools/svp_inject.py`: stock members untouched,
   and the same inputs give the same bytes. It checks the member list exactly: these additions,
   the two replaced files with their stock owner and mode, nothing removed, every other member's
   header byte for byte as in stock, and `/svp` growing by no more than 12 MB.
6. Packs the container with `tools/desay_pkg.py` on the stock header and `.md5`.
7. Verifies the result from its bytes: header changes only in the component offsets and lengths,
   the other three components stock, the `.md5` lines stock, rollback 1/1/1/5.

**Output**, in `out/<build id>/`: the image under the stock file name, its `.md5`, `RELEASE.md`
(the members added or replaced, with md5s) and `svp-tree/` (the staged `/svp` files).

**The lock.** An image ships only artifacts whose md5 is in `RELEASE.lock`; the build refuses
any other. This repository's lock lists the Public Beta 1 artifacts. A build from source differs
from them (paths, toolchain, build date), so build with `--accept-new`: it records the new md5s in
the lock, and `RELEASE.md` marks the image as containing binaries "not yet run in the car". The
lock keys are `aa-headless` and `aa-bridge`, and `<base>/<file>` for each base's rebrand and
language files.

## Boot

`init.sh` starts `/svp/bin/carlifeservice` under procguard in the CarLife slot, about 4 s into
boot and before AppConnect. The stub runs `aa_run.sh carlife`. On its first start in a boot the
launcher:
1. counts the boot (`/storage/data/data/aa/boots`);
2. looks for an `aa-off` or `aa-on` file on a card or stick;
3. decides the mode and records it in `events.log`;
4. in **aa** mode, bind-mounts the rebrand, starts the receiver and the bridge, each under its
   own procguard, then execs the shim;
5. in **stock** mode, execs the stock service under its own name.

Stock is the answer to anything unexpected:

| Trigger | What happens |
|---|---|
| The shim cannot start (port or registry) | It writes why to `/tmp/run/aa/shim.fatal` and exits 0; the stock service takes the slot for the rest of the boot |
| The shim crashes | procguard restarts it, and the crash is logged. More than 5 starts in 60 s: the stock service until the next boot |
| The receiver or the bridge crashes | Restarted, and the shim reconnects them. More than 5 starts in 60 s: 30 s between starts |
| The receiver hangs | The shim pings it every 2 s; after 3 missed answers it is killed and restarted |
| Boots keep dying early | Three boots in a row that end within 90 s, other than by a switch-off: the fourth runs stock, then Android Auto again |
| `aa-off` at the root of a card or stick | Android Auto off from the next boot (at once if seen at boot), until an `aa-on` card |
| `aa-lang-off` / `aa-lang-on` | The UI languages off (stock texts) / back on, in English |
| `aa-appearance-off` | Settings > Appearance back to the car's coding |

Nothing on a card or stick is ever read or run, apart from these names and an `aa-logs` folder.
More detail: [`docs/architecture.md`](../docs/architecture.md#launcher-and-supervision).

## Logs and state

- **Logs:** everything goes to syslog (`/var/log/messages`), tagged `aa-run`, `aa-carlife-shim`,
  `aa-receiver` and `aa-bridge`; decisions also go to `/storage/data/data/aa/events.log`, which
  survives a reboot (64 KB, one rotation).
- **Onto a card:** a card or stick with an `aa-logs` folder at its root gets each boot's logs in
  `aa-logs/log<NNNN>/` (syslog, `events.log`, the run state, `dmesg`, `ps`, `df`, `info.txt`),
  refreshed every `AA_LOG_REFRESH_SECS` while it stays in; the newest `AA_LOG_KEEP` folders stay.
- **State on the unit**, in `/storage/data/data/aa/`, written atomically: `lastmode`, `boots`,
  `events.log`, `disabled`, and Settings' `lang` and `appearance`. A factory reset or a clean
  update wipes it, and everything defaults. The start screen chosen in Settings > Appearance is
  also written to the unit's NOR flash, where the coding's choice lives.

## Configuration

`/svp/etc/aa.conf` holds the defaults; `/storage/data/data/aa/aa.conf` overrides any of them.
Both are read as `KEY=VALUE` lines, never sourced: keys `AA_*`, values limited to
`A-Za-z0-9_.,:/=+-`. Paths, sockets and test modes cannot be set there.

| Key | Default | |
|---|---|---|
| `AA_ENABLE` | `1` | `0`: the stock CarLife service, no rebrand, from the next boot |
| `AA_REBRAND` | `1` | AppConnect shows Android Auto instead of CarLife |
| `AA_LANG`, `AA_APPEARANCE` | `1` | The UI languages; Settings > Appearance |
| `AA_CAN_PORT` | `30705` | CAN data (night mode, gear, speed); `0` off |
| `AA_BT_PAIRING`, `AA_BT_A2DP_DROP` | `1` | Pair the phone through the car; drop the phone's Bluetooth audio while Android Auto plays |
| `AA_BT_LOCK` | `1` | `0`: other devices may connect while Android Auto's phone holds the phone link |
| `AA_CALL_FIX` | `1` | The car's phone screen stays behind Android Auto during a call |
| `AA_POWER_LISTEN_PORT` | `30202` | Media pauses at switch-off and resumes at switch-on; `0` off |
| `AA_LAST_MODE` | `1` | Remembers whether Android Auto had the speakers at switch-off; if not, the phone's autoplay is refused once at the next connection |
| `AA_MOVING_RESTRICTIONS` | `27` | Android Auto's driving restrictions while the car moves; `0` off |
| `AA_HIDE_NATIVE_MS` | `60000` | The phone stops sending video this long after Android Auto is hidden; `0` never |
| `AA_SEEK_MODE` | `nexthold` | A held seek key: `nexthold` (Next / Previous held), `ff`, `ffhold` or `longpress` |
| `AA_LOG_REFRESH_SECS`, `AA_LOG_KEEP` | `60`, `10` | Log copies onto a card |
| `AA_DEBUG` | `0` | More detail in the logs |
| `AA_MAX_STARTS`, `AA_BOOTLOOP_MAX`, `AA_STABLE_SECS` | `5`, `3`, `90` | The supervision limits above |

## Tests

```sh
docker run --rm --privileged --platform linux/arm64 -v "$PWD":/w debian:bookworm bash /w/sysupdate/test_sysupdate.sh
```

The launcher and the stubs run under busybox, as on the unit, against a fake procguard and
cgexec, busybox syslogd and stand-ins for the binaries, the stock service and AppConnect. The
sections cover: parsing, the first boot, crashes and their budgets, the configuration and its
override file, a missing binary, the boot-loop guard, `aa-off` / `aa-on`, the rebrand guards, one
instance per service, logging and the `aa-logs` copies, the language and Appearance switches,
and the shim's stub. `--privileged` is for the bind mounts.

## Flashing

Flashing, the switches and the way back to stock:
[`docs/releases/public-beta-1.md`](../docs/releases/public-beta-1.md).
