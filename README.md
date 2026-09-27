# ROTT-PS3

A native homebrew port of [Taradino](https://github.com/fabiangreffrath/taradino)
(Rise of the Triad) for the PS3.

## Lineage

Rise of the Triad was released by Apogee Software in 1994 and its source code
was published under the GPL in 2002. The icculus.org port brought it to modern
systems, and Taradino is the maintained continuation of that port (64-bit and
big-endian fixes, SDL2). This project adds a full PS3 platform layer on top
of it (video, audio, input, launcher, system integration):

```
Rise of the Triad (Apogee, 1994) -> icculus.org port -> Taradino -> ROTT-PS3 (this project)
```

The original Taradino README is kept as [`README.taradino.md`](README.taradino.md).

## Features

- **Games**: *The HUNT Begins* (shareware), *Dark War* and *Extreme Rise of
  the Triad*. A launcher picks the registered or the shareware game
  depending on the data found; the registered game's episodes are picked
  in its own New Game menu.
- **Video**: 720p output with the picture scaled by the RSX. Three internal
  resolutions: 320x200 (the original), 640x480, and 848x480 widescreen
  (16:9, more picture at the sides instead of a stretched one). Screen fit,
  smooth or sharp filtering, an FPS counter, and a large HUD option that
  draws the status bars at twice the size.
- **Audio**: 48 kHz audio on its own thread. Music the way a Sound Blaster
  played it (the original Apogee MIDI player and AdLib driver, on an emulated
  OPL3), or in General MIDI through a SoundFont (`.sf2`).
- **Input**: DualShock 3 with both sticks. Every action can be reassigned
  from Options, with stick sensitivity, dead zone, invert aim and swap
  sticks settings.
- **System**: saves and settings in `/dev_hdd0/data/rott/`, clean exit from
  the XMB ("Quit Game"), a log in the game's folder.

## Requirements

- A PS3 capable of running homebrew (HEN or CFW).
- Your own legally-owned copy of Rise of the Triad. **No game data is
  included in this repository or in any release build.** The classic
  *Dark War* files from the GOG or Steam releases or the original CD work, as
  does the freely distributed shareware version.
- To build from source: the [ps3dev / PSL1GHT](https://github.com/ps3dev)
  toolchain.

## Installation

1. Install `rott-ps3-1.0.pkg` from the XMB (it shows up as "Rise of the
   Triad"). This creates `/dev_hdd0/game/RISETRIAD/USRDIR/`.
2. By FTP, copy your game data into that folder:

   ```
   RISETRIAD/USRDIR/DARKWAR.WAD     <- registered game (Dark War)
   RISETRIAD/USRDIR/DARKWAR.RTL     <- registered, required
   RISETRIAD/USRDIR/DARKWAR.RTC     <- registered, required (ROTTCD.RTC or ROTTSITE.RTC also work)
   RISETRIAD/USRDIR/REMOTE1.RTS     <- required by both games
   RISETRIAD/USRDIR/EXTREME.RTL     <- optional: Extreme Rise of the Triad
   RISETRIAD/USRDIR/HUNTBGIN.RTL    <- optional: The HUNT Begins levels in the registered game

   RISETRIAD/USRDIR/HUNTBGIN.WAD    <- shareware game
   RISETRIAD/USRDIR/HUNTBGIN.RTL    <- shareware, required
   RISETRIAD/USRDIR/HUNTBGIN.RTC    <- shareware, required
   ```

   With the files of only one game there is no launcher menu, the game
   starts right away. If no game can start, a screen says which file is
   missing.

3. **Optional, General MIDI music**: copy a General MIDI SoundFont (`.sf2`)
   to `RISETRIAD/USRDIR/` and pick it in Options -> Music Synth. The first
   `.sf2` in the folder (by name) is used. The samples are kept in memory
   at about twice the file's size, so SoundFonts up to roughly 100 MB are a
   good fit. [GeneralUser GS](https://www.schristiancollins.com/generaluser.php)
   (31 MB) is a good choice; release builds may include it (see
   [Building from source](#building-from-source)).

Settings and saved games are kept in `/dev_hdd0/data/rott/darkwar/` (and
`huntbgin/` for the shareware game), so reinstalling the PKG keeps them.
The log of the last run is `RISETRIAD/USRDIR/rott_log.txt`.

## Building from source

1. Install the [ps3dev toolchain](https://github.com/ps3dev/ps3toolchain)
   (third-party, not part of this project):

   ```bash
   git clone https://github.com/ps3dev/ps3toolchain.git
   cd ps3toolchain
   export PS3DEV=/usr/local/ps3dev
   export PSL1GHT=$PS3DEV
   export PATH="$PATH:$PS3DEV/bin:$PS3DEV/ppu/bin:$PS3DEV/spu/bin"
   ./toolchain.sh
   ```

2. Clone this repository and build:

   ```bash
   git clone https://github.com/stingziz0u/rott-ps3.git
   cd rott-ps3/ps3
   make                # -> rott-ps3-1.0.pkg
   ```

   One EBOOT contains both the registered and the shareware engine (Taradino
   builds them from the same sources), so there is a single PKG.

3. Optional: to ship GeneralUser GS inside the PKG, put it in `pkgfiles/`
   (which mirrors the PKG) before `make`:

   ```bash
   git clone --depth 1 https://github.com/mrbumpy409/GeneralUser-GS
   mkdir -p pkgfiles/USRDIR
   cp GeneralUser-GS/GeneralUser-GS.sf2 pkgfiles/USRDIR/
   ```

`ps3/hosttest/` builds the same game for a PC, natively or as big-endian
PowerPC under `qemu-ppc64`, with scripted pad input and screenshots. It is a
development aid and is not needed to build the PKG.

## Controls

| Action | Default | Notes |
|---|---|---|
| Move / strafe | Left stick | D-pad moves and turns too |
| Turn | Right stick X | |
| Aim up / down | Right stick Y | In Mercury Mode it flies up / down instead |
| Fire | R2 | |
| Run | L2 | |
| Open / use | Cross | |
| Volte-face (turn around) | Circle | |
| Toggle weapon | Square | Bullet weapon / missile weapon |
| Next weapon | Triangle | Pistol, dual pistols, MP40 |
| Drop weapon | R1 | |
| Autorun | L1 | On / off |
| Center view | R3 | |
| Strafe | L3 | Held: turning strafes |
| Map | Select | |
| Fly up / fly down | (none) | Look up / down outside Mercury Mode |
| Menu | Start | Fixed |
| Menus: confirm / back | Cross / Circle | Start also goes back |

Everything except the sticks, the d-pad and Start can be reassigned in
Options -> Controller -> Customize Buttons.

## Known limitations

- The game runs at 35 frames per second, the engine's own tic rate.
- Comm-bat is single-player only (no network play).
- Mods are not supported in 1.0.
- Demos may go out of sync (a known Taradino issue).
- The SoundFont synth has no reverb or chorus.
- A saved game that does not match the level data (another episode or
  version) cannot be loaded: the load is abandoned with a message and the
  game goes back to the menu.

## Credits

- [Taradino](https://github.com/fabiangreffrath/taradino) by Fabian
  Greffrath, erysdren and contributors -- the engine this project is built
  on -- and the icculus.org port before it (Steven Fuller, Ryan C. Gordon,
  John Hall, Dan Olson).
- Apogee Software and Developers of Incredible Power, for Rise of the Triad
  and for releasing its source code.
- The Apogee Sound System MIDI player and AdLib driver by James R. Dose, as
  ported in [EDuke32](https://www.eduke32.com/) /
  [NBlood](https://github.com/nukeykt/NBlood) (Jonathon Fowler, the EDuke32
  developers, Nuke.YKT).
- [Nuked OPL3](https://github.com/nukeykt/Nuked-OPL3) by Nuke.YKT, with the
  Nuked-OPL3-fast changes by Tony Gies.
- [TinySoundFont](https://github.com/schellingb/TinySoundFont) by Bernhard
  Schelling.
- [GeneralUser GS](https://www.schristiancollins.com/generaluser.php) by
  S. Christian Collins, when included in a release build.
- The ps3dev / PSL1GHT toolchain and SDK.

## License

GPL-2.0-or-later, inherited from Taradino and the Rise of the Triad source
release. See [`LICENSE`](LICENSE). Some files carry other compatible
licenses:

- `rott/vgatext.*`: MIT (erysdren). `rott/vgafont.h`: public domain
  (Joseph Gil).
- `ps3/source/opl3.*`: LGPL-2.1-or-later (Nuked OPL3).
- `ps3/source/tsf.h`: MIT (TinySoundFont, with a big-endian fix for the PS3).
- `ps3/source/ps3_font.h`: generated from DejaVu Sans Mono (Bitstream Vera
  license).
- GeneralUser GS is not part of this repository; its own license allows
  including it with software.

No Rise of the Triad data files are included in this repository. You need
your own legally-owned copy of the game (or the shareware version).

## AI disclosure

This project's code was written collaboratively with Claude (Anthropic),
working through this port with me in real time over many sessions.
Every bit of testing and debugging were made by me on real hardware.
