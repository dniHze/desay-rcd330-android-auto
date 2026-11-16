# Public Beta 1

Wired Android Auto on the RCD 330, through the unit's own AppConnect screen, plus seventeen
UI languages and a choice of menu color and start screen. It is a system update built from VW's
own image: only the system partition gains our files, everything else is byte-identical to
stock. Android Auto is [LIVI](https://github.com/f-io/LIVI).

## Images

| Unit | Stock base | File | md5 |
|---|---|---|---|
| PQ, 6RD 035 187B | SW0317 (2020-03-03) | `6RD_035_187B_TR7247_SW0317_20200303.bin` | `ba93d0110a5042407a4b2a537a203b76` |
| MQB, 5GG 035 280D | SW0394 (2020-03-03) | `5GG_035_280D_TR8257_SW0394_20200303.bin` | `12dd37e93afd706128047a738f41eebd` |

Each image comes with its `.md5` file; the updater needs both. The images are not in this
repository. To make one from your own stock image, see [building.md](../building.md).

| Part | md5 |
|---|---|
| Receiver and CarLife shim, `aa-headless` (LIVI `daf9056` + `livi/patches`) | `e5c6a21cc47f5d7104822e63d18168d1` |
| Bridge, `aa-bridge` | `729742810a8e65dd3207b7d622af2349` |

The other artifacts' md5s are in [`sysupdate/RELEASE.lock`](../../sysupdate/RELEASE.lock).

## Before you start

- **CarLife must be enabled in the unit's coding.** Without it, AppConnect shows no tile.
- **Keep the way back at hand:** a card with the stock image of your unit and its `.md5`.
- **Mind the car's battery:** the update takes a while. Keep the battery charged, and don't
  crank the engine during the update.

## Flashing

1. Put the image and its `.md5` at the root of an SD card. Remove any `._*` files a Mac adds,
   and keep no other image on the card.
2. Boot the unit once without the card. Then insert it and start the update from
   Settings > System information > Update software.
3. If the update stands still for a long time, disconnect the car's battery for a minute with
   the card left in. The update resumes and finishes.

## What's in it

- **Android Auto over USB:** start it from AppConnect. Touch, navigation prompts, calls,
  music, the assistant with the car's microphone, and night mode all work.
- **Steering-wheel keys:**
  - Next and Previous skip a track when pressed.
  - Held, they act as the phone's own held key. YouTube Music seeks 10 seconds forward or back
    on each hold; Spotify changes track instead.
  - The phone key opens Android Auto's phone screen when there is no call.
- **Bluetooth pop-ups** don't cover Android Auto.
- **Switching apps:** leaving Android Auto for under a minute comes back straight to the live
  picture.
- **Seventeen UI languages**, in Settings > Language. English is the default.
- **Settings > Appearance:**
  - the color of the menus (amber, red, blue, or as the car is coded);
  - the start screen picture shown at boot.
- **Settings > System information** shows "Android Auto: Public Beta 1 (LIVI)".

## Known limits

- **Seeking from the steering wheel depends on the app:** a held key seeks 10 seconds per hold
  in YouTube Music, but changes track in Spotify. Android Auto gives the car no other way to
  seek. Elsewhere, seek on the screen or by voice ("Hey Google, skip forward 30 seconds").
- **Start screen picture:** not yet confirmed in a car.
- **The MQB image** has not been tried in a car yet.

## Reporting a problem

1. Make a folder named `aa-logs` at the root of an SD card or USB stick, and insert it.
2. The unit copies its logs into `aa-logs/log<NNNN>/`, one folder per boot. It refreshes them
   every minute while the card stays in, and keeps the newest ten.
3. Nothing else on the card is read or changed.
4. Open an issue with that folder and a short description of what happened and when. The logs
   hold your phone's and car's Bluetooth addresses: share them privately if you prefer.

## Switching it off, and going back

- **Android Auto off:** a card with an empty file named `aa-off` at its root turns Android Auto
  off from the next boot, and the stock CarLife service comes back. A card with `aa-on` turns
  it on again.
- **Languages and Appearance off:** `aa-lang-off` and `aa-appearance-off` do the same for them.
- **All the way back:** flash the stock image of your unit with its `.md5`.
