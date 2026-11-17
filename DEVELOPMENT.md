# Developing Wheelio

## How it fits together

- `app/` is the menu bar app, in Swift, built with Xcode from `project.yml` (XcodeGen).
- `bridge/` carries the game's force feedback to the wheel. The game loads
  `bridge/windows/dinput8_proxy.cpp` in place of DirectInput; it sends the game's effects over a
  local socket to `bridge/macos/bridge_server.cpp`, which plays them on the wheel.
- `steam/` is the patch Steam loads to switch on Steam Play (below).
- `engine/` builds the Wine engine games run on (below).
- `purr/` is a submodule of NekoLabStudio's [Purr](https://github.com/NekoLabStudio/purr), which
  provides the wheel drivers, the OpenXR runtime (built here as `wheelio_openxr.dll`) and the VR
  streamer, its own Steam Link host.

## Building

You need Xcode, Rust and `brew install xcodegen cmake mingw-w64`. Clone with
`git clone --recursive` (or run `git submodule update --init`).

```bash
./scripts/build-proxy.sh    # Windows DLLs: 64- and 32-bit proxy, OpenXR runtime
./scripts/gen-xcode.sh      # the Xcode project is generated from project.yml
xcodebuild -project Wheelio.xcodeproj -scheme Wheelio \
  -configuration Release -derivedDataPath build build
```

The app bundles the DLLs, the VR streamer and the Steam patch, and ends up in
`build/Build/Products/Release/Wheelio.app`. Local builds are ad-hoc signed, so macOS asks for the
Input Monitoring and audio permissions again after every rebuild; pass
`CODE_SIGN_IDENTITY="Apple Development: Your Name (TEAMID)"` to keep them. Never build with
`CODE_SIGNING_ALLOWED=NO`, or macOS silently refuses audio recording and the headset gets no
sound.

Until an engine is published, point the app at one you built: unpack it and run
`launchctl setenv WHEELIO_ENGINE /path/to/engine` before opening the app.

Tests:

```bash
cmake -S . -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
xcodebuild -project Wheelio.xcodeproj -scheme Wheelio -destination 'platform=macOS' test
./scripts/streamer-cargo.sh test --release
```

The CMake tests include the Steam patch's, which also check it against the Steam client installed
on the Mac, when there is one.

## The Steam patch

Mac Steam contains Linux Steam's Steam Play but keeps it off. `steam/` is a small library Steam
loads as it starts (through `LSEnvironment` in `/Applications/Steam.app`, which the app re-signs
locally, keeping Valve's copy to put back). It switches Steam Play on, and Steam then finds
Wheelio's compatibility tool in `compatibilitytools.d` and uses it for the games
`config.vdf` maps to it.

The library finds what it changes by text in Steam's own code (`steam/src/steam_anchors.c`)
rather than by addresses, so a new Steam build doesn't need a new table, and it changes nothing
unless it finds all of it. It writes what it did to
`~/Library/Application Support/Wheelio/Steam/status.json`. `wheelio-steam-check` runs the same
search over a Steam client on disk.

Mac Steam's own Add a Non-Steam Game only takes Mac apps, so Settings adds Windows games from
elsewhere itself: it writes them to the signed-in account's `userdata/<account>/config/shortcuts.vdf`
and maps them to Wheelio in `config.vdf`, both while Steam is closed. An installer runs first, in
the Wine environment Steam will give the game (`steamapps/compatdata/<shortcut id>` in Steam's own
folder), and the game is then found among the shortcuts it put in the Start menu.

## The engine

Games run on CodeWeavers' Wine, built from CrossOver's source release with the patches in
`engine/patches`, plus Valve's lsteamclient and Proton's steam helper so games can talk to Mac
Steam. It also carries MoltenVK, DXVK and DXMT for graphics, GStreamer and FFmpeg for video, and
Wine Mono and Wine Gecko. `engine/UPSTREAM.txt` pins everything the build downloads.
`engine/run` is what Steam starts for each game; it keeps each game's Wine prefix in Proton's
layout (`compatdata/<app id>/pfx`, user `steamuser`), so Steam Cloud finds Windows saves.

To build it you also need Rosetta and `brew install bison pkgconf xz meson ninja nasm glslang`:

```bash
WHEELIO_ENGINE_WORK=/path/to/work engine/build.sh
```

The work folder holds the downloads, sources and build trees (about 4 GB). A first build takes
about fifteen minutes on a Mac with many cores; later ones only rebuild what changed. The result
is `wheelio-engine-<CrossOver version>-<revision>-<arch>.tar.xz`. `WHEELIO_ENGINE_ARCH` chooses
x86_64 (the default, run under Rosetta) or arm64, which needs FEX before it can run games. Bump
`ENGINE_REVISION` in `engine/build.sh` when the engine changes.

### Graphics

DXMT runs DirectX 10 and 11 on Metal, and Wine's own OpenGL renderer runs DirectX 9 and older.
DirectX 12 needs Apple's Game Porting Toolkit (D3DMetal), which the app walks players through
adding. A game's launch options can choose otherwise:

- `WHEELIO_GRAPHICS=dxvk %command%` runs DirectX 9, 10 and 11 through DXVK on Vulkan.
- `WHEELIO_GRAPHICS=vkd3d %command%` runs DirectX 12 through Wine's vkd3d on Vulkan, without the
  toolkit. Few DirectX 12 games run this way, and DirectX 10 and 11 then use Wine's renderer.

### VR

A game gets the OpenXR runtime only when one of its arguments contains "openxr". It works with
64-bit games that render through OpenXR with DirectX 11 or 12. Head tracking is streamed;
controllers aren't, since games are played with the wheel. Apple Vision Pro isn't supported:
Purr shows games there from inside the game's own process, which a Windows game can't do.

### Logs

- Each game: `steamapps/compatdata/<app id>/wheelio.log` in its Steam library.
- The VR streamer: `streamer.log` in the game's `pfx/drive_c/ProgramData/Wheelio/VR` folder.
- Force feedback in detail: create an empty `wheelio_proxy.log` next to the game's `.exe`; the
  proxy writes to it while the game runs.
- The app: `log show --predicate 'subsystem == "uk.ivonunes.wheelio"'`.

## Updates

Two weekly workflows open pull requests for dependency updates. `update-crossover.yml` looks for
a newer CrossOver source release and rebases `engine/patches` onto it with
`engine/update-crossover.sh`; when a patch no longer applies, the pull request is a draft that
names the conflict. `update-purr.yml` moves `purr/` to Purr's latest release. Both push with the
deploy key in the `updates` environment.

## Releasing

The app downloads its engine, so the engine is released on its own. When `engine/` has changed,
run the Release engine workflow first: it builds the engine, publishes it as an
`engine-<version>` release and opens a pull request for `app/engine-release.json`, the engine
the app pins. Merge it, then run Release as usual. It stops if `engine/` has changed since the
pinned engine was built.

`main` only takes pull requests whose CI passes, with one approving review. The repository's
admins can merge without the review, but not without CI.

## Credits

- [CrossOver](https://www.codeweavers.com/crossover): CodeWeavers' Wine, and the MoltenVK, DXVK,
  GLib and GStreamer the engine builds from its source release.
- [Proton](https://github.com/ValveSoftware/Proton): lsteamclient, the steam helper, and Proton's
  changes to Wine for them.
- [NotProton](https://github.com/NotProtonNot/NotProton): showed that Steam Play can be switched
  on in Mac Steam; its macOS ports of lsteamclient and the steam helper are the starting point
  for Wheelio's.
- [DXMT](https://github.com/3Shain/dxmt): DirectX on Metal.
- [MoltenVK](https://github.com/KhronosGroup/MoltenVK) and [DXVK](https://github.com/doitsujin/dxvk):
  Vulkan on Metal, and DirectX on Vulkan.
- [GStreamer](https://gstreamer.freedesktop.org) and [FFmpeg](https://ffmpeg.org): games' videos.
- [Wine Mono](https://gitlab.winehq.org/mono/wine-mono) and
  [Wine Gecko](https://gitlab.winehq.org/wine/wine-gecko): .NET and Internet Explorer.
- [Purr](https://github.com/NekoLabStudio/purr): the wheel drivers, the OpenXR runtime and the VR
  streamer, whose Steam Link protocol follows [OpenVRLink](https://github.com/shinyquagsire23/OpenVRLink)'s
  documentation.
