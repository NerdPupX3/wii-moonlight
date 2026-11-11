# Wii Moonlight

Stream games and your desktop from a PC running [Sunshine](https://github.com/LizardByte/Sunshine)
to a Nintendo Wii, using the Moonlight protocol.

> [!WARNING]
> **This is an ALPHA release, very early in development.**
> Expect bugs, freezes, crackling audio and the occasional crash. It works,
> but it is not polished. Please [report what goes wrong](#reporting-bugs) —
> that is the most helpful thing you can do right now.

## Features

- Finds Sunshine PCs on your network automatically, or enter the IP address with an on-screen keypad
- Pairing with an on-screen PIN
- 360p (recommended) or 480p video at 30 fps, stereo audio
- Wii Remote as mouse (IR pointer) and gamepad, with Nunchuk support

## Known issues

- Audio can crackle or drop out, especially over Wi-Fi.
- The picture can freeze for about a second when Wi-Fi drops packets (480p is worse).
- The pairing screen can't be cancelled while it waits for the PIN.
- Connecting to a wrong IP address can take a long time to fail.
- The app exits after a stream ends; launch it again to start another stream.
- Only the Wii Remote (with or without Nunchuk) is supported. No Classic Controller or GameCube controller yet.

## Installation

1. Download `wii-moonlight-<version>.zip` from the [Releases](../../releases) page.
2. Unzip it to the root of your SD card, so you get `apps/wii-moonlight/boot.dol`.
3. Launch **Wii Moonlight** from the Homebrew Channel.

### Set up Sunshine on your PC

1. Install and run [Sunshine](https://github.com/LizardByte/Sunshine).
2. **In Sunshine's web UI (Configuration → Advanced), set FEC to 50.**
   This is needed even at 360p: it lets the stream recover from lost Wi-Fi packets.
3. For automatic discovery on Linux, Avahi must be running:
   `sudo systemctl enable --now avahi-daemon`, then restart Sunshine.
   Without it, enter the PC's IP address manually. It's remembered for next time.

A wired (Ethernet) connection on the Wii gives the best results, especially at 480p.

### First launch and pairing

1. The first launch creates this Wii's security key. That takes a minute or two.
2. Choose your PC from the list (or **Enter an IP address...**).
3. A 4-digit PIN appears. In Sunshine's web UI, open the **PIN** page and enter it.
4. Pick an app and stream. Press **+** in the app list to switch between 360p and 480p.

## Controls (while streaming)

| Wii Remote / Nunchuk | Sends to the PC |
| --- | --- |
| Point at the screen | Mouse pointer |
| A + C | Toggle mouse mode (A = left click, B = right click) |
| A / B | A / B |
| 1 / 2 | X / Y |
| D-pad | D-pad |
| Nunchuk stick | Left stick |
| Z / C | LB / RB |
| Z + C | LT |
| A + B | RT |
| B + Z | Left stick click |
| 1 + 2 | Right stick click |
| + / − | Start / Back |
| HOME | Guide |
| **+ and − together** | **Quit the stream** |

## Reporting bugs

Open an [issue](../../issues) and include:

- What you did and what went wrong
- `moonlight.log` from `apps/wii-moonlight/` on your SD card (it's written on every run)
- Wi-Fi or wired, 360p or 480p, and your Sunshine version

## Building from source

### 1. Install the toolchain

Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the `wii-dev` group
(devkitPPC, libogc, libfat). You also need `git`, `cmake`, `make`, `curl` and a host C compiler.
Check that `DEVKITPRO` and `DEVKITPPC` are set:

```sh
echo $DEVKITPRO $DEVKITPPC     # e.g. /opt/devkitpro /opt/devkitpro/devkitPPC
```

### 2. Get the code, with its submodules

```sh
git clone --recursive https://github.com/NerdPupX3/wii-moonlight.git
cd wii-moonlight
```

If you already cloned without `--recursive`, run `git submodule update --init --recursive`.

### 3. Build the libraries (once)

```sh
./extern/build-deps.sh
```

This builds, for the Wii:

| Library | Version | Where it comes from |
| --- | --- | --- |
| [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c) | commit `5d3c0ec` | git submodule + `patches/moonlight-common-c.patch` and `patches/enet.patch` |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | 3.6.1 | git submodule, configured by `extern/mbedtls_wii_config.h` |
| [Opus](https://opus-codec.org/) | 1.5.2 (fixed-point) | downloaded from xiph.org, checked by SHA-256 |
| [FFmpeg](https://ffmpeg.org/) | libavcodec 54.23.100 (2012) | `extern/ffmpeg`: copied from [WiiMC](https://github.com/dborth/wiimc) (`source/mplayer/ffmpeg`), H.264 decoder only, with its Wii paired-single optimisations; configured by `extern/config.mak` |

### 4. Build the app

```sh
make        # builds boot.dol
make dist   # builds dist/wii-moonlight-<version>.zip, ready to unzip onto an SD card
```

To test, copy `boot.dol` to `apps/wii-moonlight/` on the SD card, or send it over the
network with `wiiload boot.dol` while the Homebrew Channel is open.

`tools/mkfont.py` regenerates the menu font (`source/font.h`).

## Credits and license

- [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c) — the streaming protocol
- [Moonlight Embedded](https://github.com/moonlight-stream/moonlight-embedded) — the pairing / HTTP client code this is based on
- [FFmpeg](https://ffmpeg.org/) (H.264 decoding), [Opus](https://opus-codec.org/) 1.5.2 (audio), [Mbed TLS](https://www.trustedfirmware.org/projects/mbed-tls/) 3.6.1 (encryption)
- [WiiMC](https://github.com/dborth/wiimc) and Extrems for the Wii-optimised (paired-single) FFmpeg code
- Menu font: [Noto Sans](https://fonts.google.com/noto/specimen/Noto+Sans) (SIL Open Font License 1.1)

Not affiliated with Nintendo, NVIDIA, LizardByte or the Moonlight project.

Licensed under the **GNU General Public License v3.0** — see [LICENSE](LICENSE).
