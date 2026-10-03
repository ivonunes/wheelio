# Wheelio

Wheelio adds force feedback for steering wheels to Windows games you play on a Mac
through CrossOver or Wine. It can also stream those games to a VR headset.

## What you need

- A Mac with Apple silicon running macOS 15 or later
- One of these wheels:
  - Logitech G923 for PlayStation and PC
  - Logitech G923 for Xbox and PC
  - Logitech G29
  - Logitech G920

The G923 for PlayStation and PC is the one Wheelio is tested with. The others use the same
force feedback commands and should work, but haven't been tried on real hardware yet.

## Getting started

1. Download Wheelio from the [latest release](../../releases/latest) and open it. It lives in
   the menu bar.
2. Allow Input Monitoring when macOS asks. Wheelio needs it to talk to the wheel. If you
   skipped the prompt, the menu bar icon shows a warning with a button that takes you to the
   right place in System Settings. Choose Relaunch Wheelio afterwards, as the permission only
   applies once the app restarts.
3. Choose Set Up a Game… from the menu, pick your CrossOver bottle and the game's `.exe`, and
   click Install Force Feedback.
4. Start the game through CrossOver. The setup window tells you when the game has connected.

The wheel connects on its own when you plug it in.

## Settings

Choose Settings… from the menu to:

- Start Wheelio when you log in.
- Adjust the force feedback. Force, Spring and Damper scale what the game sends (100% leaves it
  as the game intended). Smoothing softens sudden changes. Min. force lifts light road detail
  above the point where the wheel starts to move.
- Test the wheel with a short pulse and an LED sweep.
- Check for updates.

## VR

Wheelio can stream games that support VR to a standalone headset over Wi-Fi, using the free
ALVR app on the headset. ALVR runs on Meta Quest, Pico and other headsets; see
[ALVR's site](https://github.com/alvr-org/ALVR) for how to install it on yours. Use ALVR
version 20.14 on the headset, as that's the version Wheelio streams with.

1. In Wheelio's settings, turn on Stream games to a VR headset.
2. Add `-openxr` to the game's Steam launch options. For ETS2 and ATS on the regular branch
   (1.55 or later), use `-experimental_vr -openxr`.
3. Start the game, put the headset on and open ALVR. It finds your Mac and connects by itself.

The game's sound plays in the headset and your Mac is muted while it does. The first time,
macOS asks whether Wheelio may record system audio, which is how the sound reaches the
headset. Your Mac and the headset need to be on the same network.

VR works with 64-bit games that render through OpenXR with DirectX 11 or 12. Head tracking is
streamed; VR controllers aren't, since you play with the wheel. The VR settings also let you
choose the video codec, bitrate and whether to mute the Mac. After you change them, the next
connection drops once and ALVR reconnects a moment later.

## Troubleshooting

If the game doesn't pick up force feedback, open `winecfg` for the bottle and check there's a
DLL override for `dinput8` set to `native, builtin`. Set Up a Game normally adds it for you.

For a detailed log, create an empty file called `wheelio_proxy.log` next to the game's
`dinput8.dll`. Wheelio writes to it while the game runs, and stops once you delete it.

## Setting up a game by hand

The release also includes `dinput8.dll` (64-bit) and `dinput8_x86.dll` (32-bit) for setups the
installer doesn't cover, such as plain Wine. Copy the one matching the game next to its
executable, rename it to `dinput8.dll` if needed, add the DLL override above, then start
Wheelio and the game.

## Development

You need Xcode, Rust and `brew install xcodegen cmake mingw-w64`.

```bash
./scripts/build-proxy.sh    # Windows DLLs: 64- and 32-bit proxy, OpenXR runtime
./scripts/gen-xcode.sh      # the Xcode project is generated from project.yml
xcodebuild -project Wheelio.xcodeproj -scheme Wheelio \
  -configuration Release -derivedDataPath build build
```

The app bundles the DLLs and ends up in `build/Build/Products/Release/Wheelio.app`. Local
builds are ad-hoc signed, so macOS asks for the Input Monitoring and audio permissions again
after every rebuild; pass `CODE_SIGN_IDENTITY="Apple Development: Your Name (TEAMID)"` to keep
them. Never build with `CODE_SIGNING_ALLOWED=NO`, or macOS silently refuses audio recording and
the headset gets no sound.

Tests:

```bash
cmake -S . -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
xcodebuild -project Wheelio.xcodeproj -scheme Wheelio -destination 'platform=macOS' test
(cd streamer && cargo test --release)
```

The VR streamer is ALVR's server core pinned to v20.14.1 in `streamer/Cargo.toml`. ALVR only
connects matching versions, so moving the pin means updating the headset version above too.
The bridge logs to the system log (`log show --predicate 'subsystem == "uk.ivonunes.wheelio"'`)
and the OpenXR runtime to `wheelio_openxr.log` next to its DLL.

