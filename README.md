# Compartilhagram Native

C++20 / Qt6 Widgets desktop client for `https://games.butecodosdevs.com`.
Uses **standalone Google libwebrtc**, through the `webrtc-sdk/libwebrtc` C++ wrapper.
There is no Qt WebEngine, Chromium UI, JavaScript runtime, or embedded web page.
Only Compartilhagram is implemented; the other games, pages, and site navigation are excluded.

## Linux build and run

The Linux target is **Linux x86_64**. Requirements: a C++20 compiler, CMake 3.24+,
Qt6 Widgets/Network/DBus/Test development packages, pkg-config, PulseAudio, PipeWire,
libyuv, SQLite3, libsecret, and OpenSSL development headers (`libpulse-dev
libpipewire-0.3-dev libyuv-dev libsqlite3-dev libsecret-1-dev libssl-dev` on Debian).
Qt Multimedia and Qt WebSockets are not required.

From the repository root:

```sh
cmake -S compartilhagram-native-app -B compartilhagram-native-app/build -DCMAKE_BUILD_TYPE=Release
cmake --build compartilhagram-native-app/build -j4
./compartilhagram-native-app/build/compartilhagram
```

The first configure downloads the pinned Linux SDK release `libwebrtc.m144.7559.09`
(approximately 9 MB), verifying its SHA-256. To use an already extracted SDK offline:

```sh
cmake -S compartilhagram-native-app -B compartilhagram-native-app/build \
  -DWEBRTC_ROOT=/absolute/path/to/linux-x64-release -DDOWNLOAD_WEBRTC=OFF
```

`WEBRTC_ROOT` must contain `include/libwebrtc.h` and `lib/libwebrtc.so` from that release.
The build executable has an RPATH to the SDK. An optional installation copies the SDK library:

```sh
cmake --install compartilhagram-native-app/build --prefix "$PWD/compartilhagram-native-app/build/install"
./compartilhagram-native-app/build/install/bin/compartilhagram
```

Qt and the system libraries reported by `ldd` still need to be installed on the target machine.

## Windows build and distribution

The Windows target is **Windows 11 x64**, with Visual Studio 2022 (Desktop
Development with C++), Windows SDK **10.0.22000.0 or newer**, CMake 3.24+, and
**Qt 6.8.3 MSVC 2022 64-bit**. Windows 10, ARM64 and MinGW are not supported by
this build configuration. Use Release or RelWithDebInfo; the pinned WebRTC C++
SDK is not compatible with the MSVC Debug runtime/STL.

Screen and window capture use the WebRTC SDK's native desktop capturer. Audio
uses WASAPI process loopback, independently of the selected window, at 48 kHz
stereo. No microphone is opened. The Windows process-loopback API includes child
processes: browsers and embedded helper processes can therefore share one audio
choice. Overlapping capture trees are grouped to avoid duplication; the app's
own received audio is excluded. Choices use executable paths and discovery
refreshes once per second. If an excluded executable becomes part of another
listed application's tree, that entire tree is omitted at the next refresh.
Inaccessible/protected processes are not listed.
Capture errors stop that audio capture and are reported; there is no fallback
to recording the entire output device. Audio sessions opened after capture
starts are discovered automatically.

The Windows build uses the official SDK codecs. The custom Linux VA-API/NVENC
adapter and its GPU telemetry are **not ported to Windows**; this build does not
promise hardware acceleration there. Standard video encoding remains available.

From a Developer PowerShell for VS 2022, install libyuv with vcpkg (the custom
triplet links its libraries statically and uses the release DLL CRT). The
workflow in `.github/workflows/windows.yml` pins the vcpkg revision; local builds
can use the same revision for reproducibility:

```powershell
git clone https://github.com/microsoft/vcpkg.git build-vcpkg
# See the workflow's vcpkg checkout ref for the pinned commit.
./build-vcpkg/bootstrap-vcpkg.bat -disableMetrics
./build-vcpkg/vcpkg.exe install libyuv:x64-windows-static-md --overlay-triplets=packaging/windows

# Replace this path with your Qt installation.
$qtPrefix = "C:/Qt/6.8.3/msvc2022_64"
$env:PATH = "$qtPrefix/bin;$env:PATH"
cmake -S . -B build-windows -A x64 `
  -DCMAKE_PREFIX_PATH="$qtPrefix" `
  -DCMAKE_TOOLCHAIN_FILE="$pwd/build-vcpkg/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
  -DVCPKG_OVERLAY_TRIPLETS="$pwd/packaging/windows"
cmake --build build-windows --config Release --parallel 4
ctest --test-dir build-windows -C Release --output-on-failure
./build-windows/Release/compartilhagram.exe
```

CMake downloads `libwebrtc-win-x64-release.zip` from the same pinned
`libwebrtc.m144.7559.09` release as Linux, verifying SHA-256
`55bde16897e83f3bcaf001ba72c2189e96197193c3160a5eeb154f769fd6551d`.
Offline builds can set `-DWEBRTC_ROOT=C:/path/to/libwebrtc-x64-release
-DDOWNLOAD_WEBRTC=OFF`. That directory must contain `include/libwebrtc.h`,
`lib/libwebrtc.dll`, `lib/libwebrtc.dll.lib` and `LICENSE`.

To distribute, install into a clean folder. CMake copies the WebRTC DLL and runs
Qt's `windeployqt` to collect Qt plugins, libraries, and compiler runtime:

```powershell
cmake --install build-windows --config Release --prefix dist-windows
Copy-Item build-vcpkg/installed/x64-windows-static-md/share/libyuv/copyright dist-windows/share/compartilhagram/libyuv-LICENSE
Copy-Item build-vcpkg/installed/x64-windows-static-md/share/libjpeg-turbo/copyright dist-windows/share/compartilhagram/libjpeg-turbo-LICENSE
Copy-Item README.md dist-windows/README.md
Compress-Archive -Path dist-windows/* -DestinationPath Compartilhagram-windows-x64.zip
```

Extract the **whole ZIP** and launch `bin/compartilhagram.exe`. Copying the EXE
alone is insufficient. The Windows workflow builds, runs the shared tests and
uploads this ZIP as an artifact; adding the workflow locally does not run it.

### Windows validation

Shared tests cover protocol, UI, WebRTC encode/decode loopback, audio mixing,
selection and process-tree grouping. Linux portal/PipeWire integration tests
remain Linux-only. Native Windows audio integration is opt-in because hosted
CI runners normally have no usable audio output device. On Windows 11 with a
working output device, run the following (it plays two short test tones):

```powershell
$env:COMPARTILHAGRAM_TEST_WINDOWS_AUDIO = "1"
./build-windows/Release/audio_test.exe windowsApplicationCapture
Remove-Item Env:COMPARTILHAGRAM_TEST_WINDOWS_AUDIO
```

The test checks discovery, inclusion, exclusion, mixed audio and stopping using
two separate WASAPI playback processes. Before releasing, also test screen and
window sharing, source switching, application exit/restart, multiple monitors
and DPI settings, system tray behavior, and a Windows-to-Linux call on a real
Windows desktop. Verify the extracted package on a machine without Qt installed.

References: [Microsoft process-loopback API](https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/),
[Qt Windows deployment](https://doc.qt.io/qt-6/windows-deployment.html).

## Single-file Linux build

After configuring the project, build the portable executable with:

```sh
# Run inside compartilhagram-native-app:
cmake --build build --target appimage -j4
./build/Compartilhagram-x86_64.AppImage
```

The `appimage` target bundles the configured SDK, Qt (including TLS, X11 and Wayland
plugins), audio/capture client libraries, and their deployable dependencies into
one file. With the system-codec SDK selected, this also bundles its FFmpeg libraries.
The first packaging run downloads checksum-verified `linuxdeploy` and its Qt plugin
into `build/appimage-tools`; subsequent runs reuse them. Packaging additionally
requires `curl`, `sha256sum`, `pkg-config`, Python 3, `ldd`, OpenSSL and X11 SM/ICE development libraries, and the
Qt Wayland runtime plugins. `QMAKE` can select the qmake belonging to a custom Qt build.
The AppImage output tool also downloads its runtime during packaging, so network
access is required unless `LDAI_RUNTIME_FILE` points to a previously downloaded runtime.
If the upstream continuous releases change, the checksum check deliberately fails;
review the new tools before updating the hashes in `packaging/build-appimage.sh`.

This is a **single-file AppImage, not a statically linked ELF**. The existing Qt and
WebRTC SDK are shared libraries. The AppImage still needs a compatible Linux x86_64
system with glibc, graphics drivers, desktop/display services, an audio server, and
the screen-capture portal for Wayland. Build on the oldest distribution you intend
to support: packaging does not make newer glibc requirements work on older systems.
System certificates and fonts remain supplied by the host. See the
[linuxdeploy packaging guide](https://docs.appimage.org/packaging-guide/from-source/linuxdeploy-user-guide.html).

On a system without FUSE, launch with:

```sh
APPIMAGE_EXTRACT_AND_RUN=1 ./build/Compartilhagram-x86_64.AppImage
```

Each packaging run uses a fresh `build/appimage-stage.*` directory, retained for
inspection. Only the final `.AppImage` needs to be copied to another machine.
The packager explicitly scans the copied Qt plugins for dependencies, including
Wayland's Qt client libraries. Before replacing the final artifact, it checks all
bundled ELF files for missing dependencies and rejects application libraries (Qt,
PipeWire, PulseAudio, WebRTC, FFmpeg, and SM/ICE) resolved from the build host rather
than the AppDir. PipeWire and SM/ICE are explicitly included despite linuxdeploy's
default exclusions. Run that check separately with
`python3 packaging/verify-appdir.py build/appimage-stage.XXXXXX/AppDir`.
Validate capture, playback, TLS, and GPU codecs on each supported target desktop.

### Clean Arch Linux container test

With Docker available, test the final AppImage in a fresh Arch Linux environment:

```sh
bash packaging/archlinux/test.sh
# Optional: choose a different artifact and output directory
bash packaging/archlinux/test.sh /absolute/path/to/Compartilhagram-x86_64.AppImage build/archlinux-test
```

The image uses the official `archlinux:base` image with updated desktop runtime
packages and a headless Weston compositor. It deliberately installs no system Qt,
WebRTC, FFmpeg, PulseAudio, or PipeWire packages. The test verifies ELF dependencies,
offscreen startup, and Wayland startup with a configured window and an attached
rendered buffer. It runs as your UID with networking disabled and no host desktop
sockets or GPU devices mounted. AppImage extraction avoids requiring FUSE.

Results, package versions, image metadata, the tested AppImage checksum, and logs
are written to `build/archlinux-test/`. The container is removed after the run; the
Docker image `compartilhagram-archlinux-test` remains available for reuse. Arch is a
rolling distribution, so later builds may test newer packages. This test does not
exercise GPU acceleration, screen-capture portals, audio playback, or authenticated
streaming sessions.

## GPU encoding and decoding (Linux)

The system-codec SDK uses **FFmpeg + the system VA-API driver for H.264 encoding and
decoding**. It prefers compatible H.264 profiles while preserving the other WebRTC
codecs for interoperability. AMD/Intel devices with a working VA-API driver can use
this backend; NVIDIA NVENC/NVDEC are not implemented. Unsupported codecs/profiles,
missing devices, permission errors, and hardware failures fall back to WebRTC's
software codec. A failure during a stream does not require reconnecting.

The persistent bottom status bar reports encoding and decoding separately. **GPU
enabled** means the codec has produced an actual frame, not merely that a GPU exists.
Software streams show **GPU acceleration not enabled — reason: …**, with their codec
implementation. Idle means no video codec is active yet. Multiple streams can use a
mix of hardware and software. Transient server messages do not replace this status.

Build the custom SDK once, then configure the app to use it:

```sh
# Requires system FFmpeg development packages:
# libavcodec-dev libavutil-dev libavformat-dev (tested with FFmpeg 7.1 on Debian 13).
./compartilhagram-native-app/sdk/build-system-sdk.sh
cmake -S compartilhagram-native-app -B compartilhagram-native-app/build \
  -DWEBRTC_ROOT="$PWD/compartilhagram-native-app/build/hardware-sdk/linux-x64-system" \
  -DDOWNLOAD_WEBRTC=OFF
cmake --build compartilhagram-native-app/build -j4
```

The SDK script pins the engine, wrapper, and build-tools revisions. Its first run
downloads several gigabytes and builds WebRTC locally; allow approximately 20 GB of
free space and sufficient RAM. `COMPARTILHAGRAM_BUILD_JOBS` controls build parallelism
(default 16). Repeated runs reuse the checkout and build outputs. The app's original
prebuilt SDK remains usable for software-only operation and reports that its system
codec adapter is missing. System FFmpeg and GPU drivers are not bundled.

For a particular GPU, launch with `COMPARTILHAGRAM_VAAPI_DEVICE=/dev/dri/renderD129`.
To force software codecs, use `COMPARTILHAGRAM_DISABLE_GPU=1`. These settings apply
to both encoding and decoding. The GPU must support the chosen H.264 profile and
frame dimensions; VA-API availability alone is not sufficient.

Capture still uploads CPU frames to the encoder, and decoded frames are downloaded
for Qt Widgets rendering. This accelerates the codecs; it is not a zero-copy GPU
capture/rendering pipeline. Audio remains Opus on the CPU.

## Session and usage

On first launch (Linux only), the app silently tries to read the **Better Auth session
cookie** straight out of your default browser's own profile — no copy-pasting needed if
you are already logged in there. It supports Gecko-based browsers (Firefox, Zen,
LibreWolf, Waterfox, Floorp — read directly, the cookie store is not encrypted) and
Chromium-based ones (Chrome, Chromium, Edge, Brave, Vivaldi, Opera — decrypted using the
key OSCrypt keeps in the desktop keyring via libsecret). The cookie database is always
copied to a private temporary file before being read, since the browser holds it open
while running, and is deleted immediately after. If no session is found this way, the
window explains that and opens the site for you to log in, then keeps retrying in the
background so it continues on its own once you're done — no extra click needed.

You can still paste the **Better Auth session cookie** manually instead. Paste either the
value or `__Secure-better-auth.session_token=value` / `better-auth.session_token=value`.
Although sometimes called a PHP-session, this server uses Better Auth, not PHPSESSID.
A bare value uses the secure cookie name required by the production HTTPS origin.
Paste percent-encoded values unchanged, including a trailing `%3D` if present.

The app validates `/api/auth/get-session` before opening the lobby. Credentials are
kept only in memory; no session is embedded in the repository or saved in settings.
Requests use the production origin, refuse redirects, and preserve TLS certificate verification.
Use **Trocar sessão** to enter another session — this always shows the manual dialog
without retrying the browser lookup, so switching accounts is a deliberate action.

The lobby shows active broadcasters, thumbnails, quality, password protection, and capacity.
Click a card to join. The viewer provides mute/volume, fullscreen, a floating window,
and the viewer roster. Closing it leaves the stream.

**Compartilhar tela** opens mode, frame-rate, resolution, privacy, password, and system-audio
settings, followed by a native screen/window picker. The server receives the start request
only after an actual captured frame arrives. Document mode uses 5 fps; media supports 30/60 fps;
resolutions are 480p/720p/1080p. The broadcast panel supports source replacement,
live quality/privacy/password updates, thumbnails, viewer lists, actual encoder statistics,
copying a share link, Discord announcements, SFU Boost, and stopping.

Choose **Tela** or **Janela** for video. On Wayland, the system permission picker opens
with the requested source type. Window capture requires a desktop portal that supports it.
Audio is selected independently in the configuration dialog:

- **Sem áudio**: no audio capture (the default).
- **Todos os aplicativos**: mix application playback.
- **Somente aplicativos selecionados**: capture only checked applications; use this for a window's app.
- **Todos, exceto os selecionados**: omit checked applications while sharing a screen or window.

Play sound in an application to make it appear in the list. You can change audio filters
while broadcasting through **Atualizar transmissão**. The broadcast panel shows an audio
level meter. Filters do not move or mute applications on your desktop, and exclude this
client's own playback to avoid sending viewers' sound back to them. The microphone is never
opened. If application capture fails, the broadcast reports an error rather than falling
back to recording all output.

The window picker does not reliably provide the application's audio identity, so select
the matching app yourself. Filters operate on application identities reported by the audio
server, not individual windows or browser tabs; multiple browser tabs may share one identity.

**Abrir link / convite** accepts the site's `?share=` and `?invite=` links. Discord
announcements happen only when you choose a channel, or start with an explicit invite.

## Media and server compatibility

- Uses the existing `screenshare:*` and `rtc:signal` events, including separate viewer/broadcaster errors.
- Socket.IO's default namespace runs over Engine.IO 4 HTTP long-polling with serialized POSTs,
  heartbeat handling, bounded queues, and reconnect backoff. Video/audio use WebRTC directly;
  they never travel through HTTP polling.
- Requests include a `Mozilla/5.0` compatibility token while identifying themselves as
  `Compartilhagram-Native`, because the existing server middleware requires that token.
  Socket authentication errors and HTTP 401/403 stop automatic retries and appear in the lobby.
  A missing lobby snapshot times out after 15 seconds; other connection failures retry at most
  five times. Use **Trocar sessão** to try again.
- ICE credentials come from `/api/rtc/ice?purpose=screenshare`, honoring server TURN settings.
  Peer offer ownership follows the web client's user-ID ordering. Early ICE candidates are queued.
- SFU publish, subscribe, and renegotiation use the existing authenticated REST endpoints.
  Subscribers retain their direct connection until the SFU answer is accepted and a video frame arrives.
  Failures report `publish_failed` / `subscribe_failed` so the server can revert viewer paths.
- Audio monitors individual playback streams through PulseAudio or PipeWire's PulseAudio service,
  applies application filters, and mixes 48 kHz stereo PCM into an independent Opus track.
  New playback streams are discovered while broadcasting and receive the same filter.
- Wayland screen/window capture uses the ScreenCast portal over QtDBus and CPU-readable
  PipeWire video buffers, converted to I420 for a custom Google WebRTC video source. The SDK's
  unsupported Wayland window enumerator is never called. X11 uses the SDK's native desktop
  capturer with separate screen/window lists. Availability depends on the compositor/portal.
- After a signaling disconnect, active media is stopped and the lobby reconnects automatically.
  Start/join again after reconnect; capture is not silently resumed.

Native differences: there is no Chrome-tab picker or tab-only audio capture, no restoration
of browser sessionStorage/PiP state, and no operating-system URL-handler registration. Paste
share/invite links inside the app. Windows x64 has a separate native capture/audio backend (see the Windows build section). macOS is not configured.

## Verification

```sh
ctest --test-dir compartilhagram-native-app/build --output-on-failure
```

The tests require permission to open local TCP/UDP sockets. They use an in-process fake HTTP
server for real authentication/polling requests, and real libwebrtc peer connections for synthetic
video encoding/decoding and Opus packet reception. Qt UI tests run offscreen and check the
session prompt, lobby states, and cancellation of a late start acknowledgment. They produce
`build/ui-preview.png` and `build/session-preview.png` using test data.

Audio tests verify include/exclude filters and mixing using two synthetic tones on an isolated
PipeWire server and virtual sink. They require `pipewire`, `pipewire-pulse`, `wireplumber`
(with its policy-only profile), `paplay`, and `pactl`; they skip the integration case if these
tools are missing. They do not record desktop audio. Portal tests use `dbus-run-session` and
a fake portal to check source type requests, cancellation, and unsupported window capture.
Pixel tests cover channel order, bounds, and odd-sized images converted to WebRTC I420.

The regular media test deliberately disables GPU use and verifies software fallback.
To exercise the real GPU with synthetic video (no desktop capture):

```sh
QT_QPA_PLATFORM=offscreen COMPARTILHAGRAM_TEST_GPU=1 \
  ./compartilhagram-native-app/build/media_test nativeEncodeDecode
```

This checks actual hardware encoding and decoding, 720p/1080p frame-size changes,
and recovery to software encoding after an induced device initialization failure.
It was verified on the AMD VA-API backend on this machine. These checks do not
establish browser/SFU interoperability or performance on every GPU and driver.

The project builds with Qt 6.8.2 / GCC 14 on Debian 13. These automated tests do not establish
end-to-end production SFU interoperability, real desktop-portal capture, or audible playback
on every audio device. Validate a native-to-web and web-to-native session on the target desktop,
including protected joins, source replacement, system audio, Boost, and disconnects.

## Code map

- `src/MainWindow.*`: native widgets, lobby/broadcast/viewer state, SFU orchestration.
- `src/ServerClient.*`: memory-only authentication, REST, and Socket.IO polling.
- `src/RtcEngine.*`: Google WebRTC peers, capture, frame rendering, encoder controls, audio playback.
- `src/SystemAudio.*`: per-application playback capture, filtering, mixing, and metering.
- `src/PortalCapture.*`: Wayland screen/window permission flow and PipeWire video capture.
- `src/CodecStatus.*`: actual codec status and software fallback reasons.
- `sdk/`: reproducible system-codec SDK build and FFmpeg VA-API adapters.
- `cmake/WebRTC.cmake`: pinned SDK acquisition and imported library target.

Protocol references: [Engine.IO 4](https://socket.io/docs/v4/engine-io-protocol/),
[pinned standalone SDK](https://github.com/webrtc-sdk/libwebrtc/tree/libwebrtc.m144.7559.09),
and this repository's `packages/client/src/modules/compartilhagram`, `packages/client/src/lib/rtc`,
and `packages/server/src/modules/compartilhagram`.
