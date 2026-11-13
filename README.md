# Crackdown: ReActivated

Crackdown and Crackdown 2 (Xbox 360) recompiled to run natively on the PS5, and Crackdown 3 (PC) run through
Prospero Win, all in one app, by ChaosTheory1075.

> Built on [elripalda's GTA IV for PS5](https://github.com/elripalda/gta4-ps5) and
> [saulob's Crackdown Recompiled](https://github.com/saulob/crackdown-xbox360-recompiled).
> All the credit goes to them. Go star the original projects and support them.

An unofficial fan project, not affiliated with or endorsed by Microsoft, Realtime Worlds or Sony.
**No game files are included:** you need your own copies of the games.

## What's in it
- Crackdown with Title Update 6, recompiled to run natively on the PS5 (not emulated)
- 60 FPS frame pacing, with a 30 FPS mode
- FSR 1 upscaling to your TV (with a sharpness setting), SMAA or FXAA, and anisotropic filtering (2x to 16x)
- Every language on the disc: English, French, German, Italian, Spanish (Spain and Latin America), Polish, Czech, Hungarian, Slovak, Japanese, Korean and Chinese, or the PS5's own
- Portuguese (Brazil) for the app's menus; the game's own text follows once a pt-BR text pack is installed (the game has a pt-BR slot the disc never filled)
- PlayStation button prompts in the game's text and menus (or the Xbox 360's, in SETTINGS)
- DualSense features: the light bar in the Agency's green (red when you're hit), adaptive triggers for driving and shooting, a kick with each shot and hit
- An FPS counter (simple or detailed) and Skip Intro
- Cheats in an in-game quick menu (L3 + R3): infinite health, ammo and vehicle health, no fall damage, movement, jump and vehicle speed multipliers, and the agility and hidden orbs you haven't collected yet marked on the minimap and the pause map (from Crackdown Recompiled)
- Both DLC packs: the Free-For-All Pack and the Four-Play Pack
- The game's achievements as PS5-style trophies, with their own page in the menu
- Installs the game, Title Update 6 and the DLC from your own files, at the drive's full speed
- Imports your Xbox 360 saves, backs up and restores saves
- A menu with HOME, CONTENT, SAVES, SETTINGS, TROPHIES and CREDITS pages, its own art and music

**Crackdown 2** (Title Update 5), picked on HOME: 60 FPS (or 30), FSR 1, SMAA, PlayStation button prompts, the
DualSense light bar and rumble, Skip Intro, the quick menu (L3 + R3: infinite health and ammo), its DLC, trophies,
Brazilian Portuguese text, and its own settings page.

**Crackdown 3** (the campaign), picked on HOME: your own PC copy run by Prospero Win. The app installs it from
your files, signs you in to Xbox once (a code on the TV you enter on your phone), keeps your saves in the cloud,
unlocks its achievements as trophies, and shows **PlayStation button prompts** (made on your console from your
own game files). The touch pad opens the map.

## What you need
- A jailbroken PS5 that runs homebrew apps, with a payload loader on port 9021 (etaHEN or elfldr)
- Your **own** Crackdown for the Xbox 360, as an `.iso` or the disc's files
- Title Update 6 (required). The base game and other title updates (TU5, ...) aren't supported
- Optional: the two DLC packs
- About 15 GB free while installing, about 6.7 GB once the disc image is deleted
- Crackdown 2 (optional): your own disc image and its Title Update 5 (`TU_16L625S_0000004000000.0000000000181`,
  3,317,760 bytes); optional DLC: the Toy Box, Deluge and Pre-Order Armor packs
- Crackdown 3 (optional): [Prospero Win](https://github.com/mpereiraesaa/prospero-win) installed (Recomp store),
  your own Crackdown 3 dumped with UWPDumper on a Windows PC, and the Windows files the PC helper
  (`pc-helper/cd3_windows_files.py` in the release zip) takes from Microsoft's Windows 11 evaluation ISO

## Which files work

| | File | Size |
|---|---|---|
| Disc image | `Crackdown (World) (En,Ja,Fr,De,Es,It,Zh,Ko,Cs,Hu).iso` (any name) | 7,835,492,352 bytes |
| Title Update 6 | `TU_16L61US_0000004000000.00000000001G1` ("Crackdown Title Update #6") | 2,453,504 bytes |
| Free-For-All Pack | `AA10729E62721B4EAF674486684E31CD9B94AC4F4D` | 92,962,816 bytes |
| Four-Play Pack | `E4DDBC100F97B6954B84E82ED807E79B5A4314C04D` | 9,089,024 bytes |

Crackdown had one release for every region. The packages have no file extension; that's normal.
Any file name works: the app tells what each file is from what's in it.

## Install
1. Copy the `PPSA53070` folder from the release `.zip` to the PS5, wherever your homebrew loader
   looks (`/data/homebrew/`, `/mnt/ext0/homebrew/`, `/mnt/usb0/homebrew/`, ...). The folder can be
   renamed.
2. Put your disc image (or the disc's files), Title Update 6 and the DLC packages in
   `PPSA53070/install/`. Extract any `.7z`, `.rar` or `.zip` on a PC first.
3. Open **Crackdown: ReActivated**. It installs everything it finds (the game takes about a minute
   with a payload loader running), then asks whether to delete the files it installed from.
4. When PLAY lights up, you're set.

To import a save, put the Xbox 360 save package (`Crackdown.sav`, a `CON` file) in `install/` and
import it from the SAVES page. It loads the next time you press PLAY.

See [package/README.txt](package/README.txt) for the rest: the menu's pages, your data and
backups, updating to a new build.

## Build from source
The build runs on Ubuntu 24.04 (WSL works) with
[ps5-plume-triangle](https://github.com/Nazky/ps5-plume-triangle) built and gta4-ps5's framework set up:

```
ci/local_build.sh <work dir> <this repo> <gta4-ps5 framework> <ps5-plume-triangle> \
                  <your default.xex> <your TU6 Default.xexp>
```

It checks your `default.xex` and TU6 `Default.xexp` against these SHA-256 values, generates the
recompiled code from them (it stays on your machine), and builds and packages the app folder:

- `default.xex`: `7aaa7a8920d1c3c737e58cf583debafe6013be0a96bec3ec136e5de7e3377e5a`
- TU6 `Default.xexp`: `a0d4fe06d6a4a2fd96d18f5de426f761fe1b8cce5d5bc7f981d738c3519c819e`

Inside the repo:

| Path | What it holds |
|---|---|
| `crackdown-recomp/` | The PS5 app: frame pacing, menu, installer, trophies, saves |
| `ps5/payload/installer/` | The installer payload (GPL-3.0, from GTA IV for PS5) |
| `ci/` | The build, the framework patches, the packaging |
| `package/` | The READMEs and the app's art and music |

## Credits
- **ChaosTheory1075**: Crackdown: ReActivated
- **[elripalda](https://github.com/elripalda/gta4-ps5)**: GTA IV for PS5, the PS5 port this one is built on.
  It provides the PS5 runtime, graphics and sound, the menu, the trophies and the installer payload
- **[saulob](https://github.com/saulob/crackdown-xbox360-recompiled)**: Crackdown Recompiled, the PC
  recompilation. It provides the game fixes, the 60 FPS frame pacing and the configuration
- **[pacific-city-recompiled / Unowa001](https://github.com/pacific-city-recompiled/crackdown-2-recomp)**:
  Crackdown 2 Recomp, the PC recompilation of Crackdown 2. It provides Crackdown 2's 60 FPS game clock
  (Copyright (c) 2026, Jerrod (Unowa001), BSD 3-Clause License:
  [LICENSES/crackdown-2-recomp-BSD-3-Clause.txt](LICENSES/crackdown-2-recomp-BSD-3-Clause.txt))
- **[designgears](https://github.com/designgears/wine-uwp)**: wine-uwp, Wine for UWP apps, which runs Crackdown 3
- **[mpereiraesaa](https://github.com/mpereiraesaa/prospero-win)**: Prospero Win, Windows programs on the PS5
- **OZORDI** and **vaduur**: Liberty Recompiled and LibertyRecompAndroid, which GTA IV for PS5 starts from
- **ReXGlue** and the **Xenia** team
- **BlackBearReloaded**: PS5 AT9 Converter (the home screen music)
- Special thanks to **Realtime Worlds**, for making Crackdown

Crackdown and all related names belong to their owners.

## License
The code in this repository is licensed under the GNU General Public License v3.0 (see
[LICENSE](LICENSE)), as GTA IV for PS5 is. Third-party components keep their own licenses. The
build also uses saulob's Crackdown Recompiled, which is licensed under GPL-2.0-only. Crackdown 2's 60 FPS
game clock comes from Crackdown 2 Recomp (pacific-city-recompiled), BSD 3-Clause: its notice is in
[LICENSES/](LICENSES/). Crackdown 3's Wine
(`cd3data/overlay` in the app) is LGPL-2.1-or-later, as Wine is: its source is in [cd3/](cd3/README.md). No game
files are included, and the games' assets are not covered by this license.
