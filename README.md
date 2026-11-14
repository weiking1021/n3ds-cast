# n3ds cast

Streams a New 3DS (video and game audio) to a TV through Google Cast, or to a browser on a PC. It captures whatever
the console shows, JPEG-encodes it on the console and sends it to a receiver page.

Requires a **New 3DS / New 2DS XL running Luma3DS** (the background process uses Luma's extended system calls to read
the running game's framebuffer and audio). Nothing is injected into the game: no hooks, no plugins.

## How it works

* **Frontend** (`3ds/app/frontend.c`, `ui.c`): the touch UI. Find a TV, change settings, start/stop. It starts the
  background process and only talks to it over IPC, so the stream keeps running after the frontend closes and while a
  game is open.
* **Background process** (`3ds/app/service*.c`, embedded in the frontend and created with `svcCreateProcess`): captures
  the game's framebuffers, encodes JPEGs, samples the DSP for audio and sends everything to the receiver over a Cast
  (CASTV2/TLS) connection.
* **Receiver** (`receiver/`, TypeScript built with Vite): the page that shows the stream. It runs on a TV through the Cast SDK, or in a
  browser through the PC receiver (`receiver/server.js`).

Video uses partial updates (only changed 16x16 tiles are re-encoded and sent). Quality can be fixed or **Auto**, which
picks it per frame from how much of the picture changed and the link's byte budget. Cast messages are limited to
64 KB and the console's Wi-Fi carries roughly 0.8 MB/s, which bounds what is possible.

## Build

Everything builds in Docker (devkitPro toolchain pinned by digest); nothing but Docker is needed on the host. The
scripts leave no image behind (`build.bat` on Windows):

```
./build.sh                # out/n3ds-cast.cia and out/n3ds-cast.3dsx
./build.sh test           # unit and integration tests
./build.sh format-check   # clang-format check
./build.sh formatted      # reformat the C sources in place
```

Optional: `docker build -f Dockerfile.build --target out --build-arg APP_ID=XXXXXXXX --output type=local,dest=out .`
bakes in your own Google Cast receiver application ID (default in `3ds/app/protocol.h`).

The `.cia` is assembled in the last Docker stage with `makerom` (3DSGuy/Project_CTR v0.19.0) and `bannertool`
(carstene1ns 1.2.3), downloaded during the build and checked against pinned SHA-256 hashes. They run in an
`ubuntu:24.04` stage because they need a newer glibc than the devkitPro image has.

### CIA packaging files (`3ds/cia/`)

* `app.rsf`: the `makerom` title description (title id, memory, CPU, system-call and service access). It allows every
  regular system call because the frontend uses debug-level ones (open/create process).
* `assets/`: HOME-menu icon (`icon.png`, 48x48), banner picture (`banner.png`, 256x128) and banner sound
  (`banner.wav`: 16-bit PCM, stereo, shorter than 3 seconds, which HOME requires of a banner sound). Replace the files to change the look; nothing else needs to change.

**The title id `0x000400000C3D5A00` (`app.rsf`, `UniqueId: 0xC3D5A`) must equal `NATIVE_CIA_TITLE_ID` in
`3ds/app/capture.c`.** The background process uses it to avoid mistaking the frontend itself for the game.

### Upload to a console (optional, separate from the build)

`Dockerfile.deploy` wraps `3ds/deploy/deploy.py` and has nothing to do with the build above:

```
./deploy.sh               # uploads out/ (deploy.bat on Windows); or give another folder: ./deploy.sh path/to/folder
```

`n3ds-cast.3dsx` goes to `/3ds` and `n3ds-cast.cia` to `/cias` on the console, which must run an FTP server (for example
ftpd). The console is `3ds-ftpd.local`, port 5000; set `N3DS_FTP_HOST` / `N3DS_FTP_PORT` to change that. Each file is uploaded
under a temporary name, read back and compared before it replaces the old one.

### Continuous integration

`.github/workflows/build.yml` runs on every push and pull request: `./build.sh format-check`, `./build.sh test`, the receiver tests, and then `./build.sh`; the `.cia` and `.3dsx` are attached to the run as the `n3ds-cast` artifact.

## Install and use

Copy the `.cia` to the SD card and install it with FBI (or run the `.3dsx` from the Homebrew Launcher). Then:

1. Start the app; the TV list fills in (Cast devices on the LAN, plus the PC receiver if one runs).
2. Select a TV and start streaming, go to HOME and open a game. The *game* is captured, not the app.
3. Quit and start the app again: it reconnects to the running background process.

Starting an NDS game ends the stream: DS mode reboots the console into a different firmware and every 3DS-mode
process, including the background one, is gone.

## PC receiver

A browser tab can replace the TV. `receiver/server.js` advertises itself by mDNS as a Cast device, speaks CASTV2 over
TLS on port 8009 exactly like a Chromecast and serves the built page (`receiver/dist`).

```
cd receiver
npm install            # once
npm start              # builds the page (receiver/dist), then runs the PC receiver; open http://localhost:8080
```

Open the tab first, then on the 3DS pick **PC Receiver (dev)** in the TV list (or enter the PC's IP) and start
streaming. Click "enable audio" in the tab for sound (browser autoplay rule).

Options: `--name "..."`, `--ip <this PC's address>`, `--peer <3DS address>` (pick the adapter in its subnet),
`--http 8080`, `--cast 8009` (the 3DS only lists devices on 8009), `--no-mdns`, `--page <folder of the built page>`.

* On a TV the page uses the real Cast SDK; opened from `localhost` (or with `?dev=1`) it uses a small WebSocket
  stand-in with the same calls. `?dev=0` forces the real SDK.
* A `STOP` from the sender (the 3DS sends it before relaunching) reloads the page, like a TV starting fresh.
* Windows asks once for firewall access for node.exe: allow **private** networks (TCP 8009/8080, UDP 5353).
* Several network adapters (VPN, WSL, VirtualBox)? The chosen one is printed; override with `--ip`.
* Protocol, reassembly, compositing and layout can be tested on a PC. **Speed cannot:** a PC browser is far faster
  than a TV's, so jank / decode / render times must still be read on the TV.

## On a TV

The page is TypeScript (`receiver/src/`) built with Vite: `npm run build` in `receiver/` makes `receiver/dist/` (type check, then bundle); with only Docker installed, `docker build -f Dockerfile.build-receiver --output type=local,dest=out/receiver .` makes the same into `out/receiver/`. Every URL in it is relative, so it works from any path.

`.github/workflows/pages.yml` does this and publishes the page to GitHub Pages whenever `receiver/` changes on `main`, after `npm test` (which builds the page) passes. Enable it once with Settings → Pages → Source: GitHub Actions, then register the page's URL as the receiver URL of your Cast application (Google Cast SDK Developer Console) and put the application ID in the 3DS build (`--build-arg APP_ID=...`).

The page speaks English and Traditional Chinese and picks the language by itself: `?lang=en` or `?lang=zh-TW` in the address first (for trying), then the language the 3DS program asks for (a `lang` field in its `CAPS` message), then the preferences of the browser or the TV, and English when none of them matches. Technical readouts (FPS, sizes, timings, the 3DS debug panel) stay English. The texts are in `receiver/src/i18n.ts`.

The page shows its own version (`version` in `receiver/package.json`) and, once a 3DS is connected, the version of the 3DS program. The two are versioned independently.

## Tests

`./build.sh test` runs the console program's tests (`3ds/app/test.sh`; sources in `3ds/app/test_*` and `3ds/tests/`): the
Cast client against a scripted receiver, the JPEG encoder against a reference, discovery, settings, UI layout, screen
capture, the launcher and the control service. The receiver has its own end-to-end test (launch/STOP, frames, mDNS) and
a stand-in 3DS:

```
cd receiver
npm test
node test/fake-sender.js --kb 60 --fps 10 --seconds 20   # plays the 3DS: same messages, incl. FRAMEPART for big frames
```

## Repository layout

Everything that runs on the 3DS is under `3ds/`; everything that shows the stream is under `receiver/`.

| Path | Contents |
| --- | --- |
| `3ds/app/` | The console program: frontend (touch UI), background process (`service_*.c`, shared declarations in `service_internal.h`), screen capture, settings, translations, the **`Makefile`**, and the native tests (`test.sh`, `test_*`) |
| `3ds/shared/` | Code used by both the frontend and the background process: Cast client and protocol, JPEG encoder, audio codec, config (`.c` and `.h` together) |
| `3ds/VERSION` | The version of the 3DS program |
| `3ds/cia/` | CIA packaging: `app.rsf`, icon, banner picture and sound |
| `3ds/tests/` | Host tests of the shared encoder code (`3ds/shared/capture_jpeg.c`) |
| `3ds/deploy/` | `deploy.py`: uploads a build to a console over FTP (run it with `deploy.sh`) |
| `receiver/` | The receiver page (`index.html`, `src/`), the PC receiver (`server.js`) and their tests |
| `Dockerfile.build` | The whole build: toolchain, tests, formatting, CIA packaging |
| `Dockerfile.build-receiver` | Builds the receiver page for a TV |
| `Dockerfile.deploy` | The environment for `deploy.py` |
| `build.sh`, `build.bat`, `deploy.sh`, `deploy.bat` | The only entry points: build/test/format and upload, Docker only |
| `out/` | Build output (`.cia`, `.3dsx`); not in git |

## Where to change what

| I want to change | Edit |
| --- | --- |
| Compiler flags, libraries, which source files are built | `3ds/app/Makefile` (`CFLAGS`, `LIBS`, `SERVICE` = files of the background process) |
| The Cast receiver application ID | `--build-arg APP_ID=` (see Build), default in `3ds/app/protocol.h` |
| Toolchain version, packaging tool versions | `Dockerfile.build` (pinned by digest / SHA-256) |
| Title id, memory, system-call access of the installed title | `3ds/cia/app.rsf` (keep the title id in sync, see above) |
| HOME-menu icon, banner, banner sound, app name and description | `3ds/cia/assets/` and the `smdhtool` line in `Dockerfile.build` |
| Version of the 3DS program (`1.2.3`) | `3ds/VERSION`: the CIA version, the app description, the number shown in the app and the one it sends to the receiver |
| Version of the receiver | `version` in `receiver/package.json`: shown by the PC receiver and on the receiver page |
| Code style | `.clang-format` |

## Third-party

UI font: Noto Sans TC Regular, SIL Open Font License 1.1 (`3ds/app/assets/OFL.txt`, `COPYRIGHT.txt`). Source:
https://github.com/notofonts/noto-cjk/tree/main/Sans/SubsetOTF/TC (source OTF SHA-256
`5bab0cb3c1cf89dde07c4a95a4054b195afbcfe784d69d75c340780712237537`). The subset in `ui-font.bcfnt` covers the glyphs
listed in `glyphs.txt`; it was generated with `mkbcfnt -s 22 -w glyphs.txt -o ui-font.bcfnt NotoSansTC-Regular.otf`
and is embedded in the `.3dsx`, so no extra SD file is needed.
