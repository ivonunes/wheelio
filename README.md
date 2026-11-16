# Wheelio

Wheelio lets you play Windows driving and sim games from Steam on your Mac, with force feedback
on your steering wheel and, if you like, in a VR headset.

## What you need

- A Mac with Apple silicon running macOS 15 or later
- Steam for Mac
- A force feedback wheel from Logitech, Thrustmaster, Fanatec or Simagic

<details>
<summary>Supported wheels</summary>

- Logitech G923, G29, G920, G27, G25, Driving Force GT, Driving Force Pro, Driving Force,
  MOMO Force, MOMO Racing, G PRO and RS50
- Thrustmaster T150, TMX, T300RS, T248, TX, TS-XW and TS-PC Racer
- Fanatec CSL Elite, CSL DD, ClubSport V2 and V2.5, Podium DD1 and DD2, CSR Elite and the
  Porsche 911 wheel
- Simagic Alpha, Alpha Mini, Alpha U and EVO

Wheelio is tested with the Logitech G923. The others should work but haven't been tried yet.

</details>

## Getting started

1. Download Wheelio from the [latest release](../../releases/latest) and open it. It lives in
   the menu bar.
2. Allow Input Monitoring when macOS asks, then choose Relaunch Wheelio from its menu.
3. Choose Settings… and turn on Play Windows games in Steam. If Steam is open, quit it.
4. Open Steam. Windows games now install and play like Mac ones.

Some games, such as Euro Truck Simulator 2 and American Truck Simulator, also have a Mac version,
which Steam installs by default. Wheelio's force feedback and VR need the Windows version, so add
those games under Force Windows version in Settings. Paste a game's Steam store link there before
installing it, and Steam only downloads the Windows version.

## VR

Wheelio can show games in a Meta Quest, or another headset with Valve's Steam Link app.

1. Add `-openxr` to the game's launch options in Steam. For ETS2 and ATS, use
   `-experimental_vr -openxr`.
2. Start the game, put the headset on and open Steam Link. Choose your Mac, which has "(VR)"
   after its name.
3. The first time, type the PIN the headset shows into the window Wheelio opens.

Your Mac and the headset need to be on the same network. The game's sound plays in the headset.

## Good to know

To run Windows games, Wheelio switches on Steam Play, which Mac Steam already contains but keeps
off. That means signing Steam again on your Mac, so macOS may ask once more for permissions you
gave Steam. Turning the setting off puts Valve's own copy of Steam back.

When Steam updates to a version Wheelio doesn't support yet, Steam carries on as normal and
Windows games wait until Wheelio catches up. Settings shows when that's the case.

For DirectX 12 games, add Apple's Game Porting Toolkit in Settings, which walks you through
downloading it from Apple.

## Troubleshooting

Each game keeps a log at `steamapps/compatdata/<app id>/wheelio.log` in its Steam library. If a
game looks wrong or runs slowly, try `WHEELIO_GRAPHICS=dxvk %command%` in its launch options.

## Credits

Wheelio runs games on CodeWeavers' Wine from [CrossOver](https://www.codeweavers.com/crossover),
whose sales fund much of Wine's work on the Mac, with Valve's
[Proton](https://github.com/ValveSoftware/Proton) connecting games to Steam.
[NotProton](https://github.com/NotProtonNot/NotProton) showed Steam Play can be switched on in
Mac Steam. The wheel drivers and VR come from [Purr](https://github.com/NekoLabStudio/purr).
[DEVELOPMENT.md](DEVELOPMENT.md) lists every component, and how to build Wheelio.
