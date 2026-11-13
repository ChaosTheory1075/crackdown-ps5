# RecompStore update: Crackdown: ReActivated 2.0.0

For the existing store entry (https://recompstore.com, PS5, Game port, title ID PPSA53070). Paste each part into
its field; the 1.1.0 text is replaced.

## The basics

| Field | Value |
|---|---|
| Name | Crackdown: ReActivated |
| Made by | ChaosTheory1075 |
| GitHub repository | https://github.com/ChaosTheory1075/crackdown-ps5 (release: https://github.com/ChaosTheory1075/crackdown-ps5/releases/tag/v2.0.0) |
| Release file | `Crackdown-ReActivated-2.0.0.zip` (its `PPSA53070/` folder, and `pc-helper/` for Crackdown 3) |
| Version | 2.0.0 |
| Title ID | PPSA53070 |

## Its page in the store

**One-line summary**

Crackdown and Crackdown 2 recompiled to run natively on the PS5 at 60 FPS, and Crackdown 3 through Prospero Win, in one app.

**Description**

The Crackdown trilogy on the PS5. Crackdown (2007) and Crackdown 2 (2010) are recompiled from their Xbox 360 code to
run natively; Crackdown 3 (2019) is your own PC copy, run by Prospero Win. Pick the game on the app's HOME page.

- Native PS5 app, not emulation: Crackdown (Title Update 6) and Crackdown 2 (Title Update 5) recompiled for the PS5
- 60 FPS in both Xbox 360 games, with a 30 FPS mode
- FSR 1 upscaling with a sharpness setting, SMAA or FXAA, and up to 16x texture filtering
- PlayStation button prompts in all three games (Crackdown 3's made on your console from your own game files)
- DualSense: the light bar in each game's colour, adaptive triggers and rumble in Crackdown
- A cheat quick menu on L3 + R3: infinite health and ammo in both Xbox 360 games; in Crackdown also vehicle health,
  no fall damage, speed and jump multipliers, and the uncollected agility and hidden orbs marked on the map
- Every DLC: Crackdown's Free-For-All and Four-Play packs, Crackdown 2's Toy Box, Deluge and Pre-Order Armor
- Every language on the discs, or the PS5's; the app's menus and both games' text also in Brazilian Portuguese
- Crackdown 3's campaign: installs from your own dump, signs you in to Xbox once with a code on your phone, keeps
  your saves in the Xbox cloud and unlocks its achievements as trophies. The touch pad opens the map
- Installs itself from your own files at full drive speed through your payload loader
- Imports your Xbox 360 saves, backs up and restores saves; the achievements as trophies with their own page
- An FPS counter, Skip Intro, and a menu to install, uninstall and manage everything

An unofficial fan project, not affiliated with or endorsed by Microsoft, Realtime Worlds, Ruffian, Sumo Digital,
Reagent Games or Sony. No game files are included.

**What players need**

- A jailbroken PS5 with a payload loader on port 9021 (etaHEN or elfldr)
- Crackdown: your own `.iso` (`Crackdown (World) (En,Ja,Fr,De,Es,It,Zh,Ko,Cs,Hu).iso`, 7,835,492,352 bytes) and
  Title Update 6 (the store can download it); optional: its two DLC packs
- Crackdown 2 (optional): your own `.iso` (`Crackdown 2 (World) (En,Ja,Fr,De,Es,It,Zh,Ko,Pl,Ru,Cs,Hu).iso`,
  7,838,695,424 bytes) and Title Update 5 (the store can download it); optional: its three DLC packs
- Crackdown 3 (optional): Prospero Win installed (from this store), your own Crackdown 3 (Microsoft Store / Game Pass)
  dumped with UWPDumper on a Windows PC, the folder `pc-helper/cd3_windows_files.py` makes from Microsoft's Windows 11
  evaluation ISO (Python 3 and 7-Zip), and an Xbox account
- About 15 GB free per Xbox 360 game while it installs (the disc image and the installed game side by side)

**Credits**

- ChaosTheory1075: Crackdown: ReActivated
- elripalda: GTA IV for PS5, the PS5 port this one is built on (runtime, graphics, sound, menu, trophies, installer payload)
- saulob: Crackdown Recompiled, the PC recompilation of Crackdown (game fixes, 60 FPS, cheats, configuration)
- pacific-city-recompiled / Unowa001: Crackdown 2 Recomp, the PC recompilation of Crackdown 2 (its 60 FPS game clock)
- mpereiraesaa: Prospero Win, which runs Crackdown 3 on the PS5
- designgears: wine-uwp, Wine for Microsoft Store apps
- OZORDI and vaduur: Liberty Recompiled and LibertyRecompAndroid, which GTA IV for PS5 starts from
- ReXGlue, the Xenia team and Wine
- BlackBearReloaded: PS5 AT9 Converter (the home screen music)
- Special thanks: Realtime Worlds, Ruffian Games and Sumo Digital, for making Crackdown 1, 2 and 3

**Web links**

- https://github.com/ChaosTheory1075/crackdown-ps5
- https://github.com/saulob/crackdown-xbox360-recompiled
- https://github.com/pacific-city-recompiled/crackdown-2-recomp
- https://github.com/mpereiraesaa/prospero-win
- https://github.com/elripalda/gta4-ps5

**Pictures**

In `Downloads/Crackdown/Extras/recompstore/`:
- **Backdrop (new): `backdrop-trilogy-3840x2160.jpg`**: the three games side by side (Crackdown, Crackdown 2,
  Crackdown 3) with the ReActivated emblem and "THE TRILOGY - NOW ON PS5", 4K 16:9. Also use it as the first
  screenshot (1080p copy: `preview-trilogy.jpg`).
- Icon `icon-512.png`, logo `logo-transparent.png` and the 1.1.0 PS5 screenshots stay.

## Anything else? (the store's setup for Your game files)

Everything is copied as is into the app's `install/` folder (`<app>/install/`); the app tells the files apart by
what's in them, whatever their names, and asks before deleting them after installing.

Crackdown (as in 1.1.0):
- **Game (required):** the disc image (kind `file`, ext `iso`), or a folder of the disc's files with `default.xex`.
- **Title Update 6 (required):** kind `stfs`, `raw: true`, `to: "install"`: title ID `4D5307DC`, content ID
  `65eda8ad841eb6a5b03f94807888ff026b5577ff`, SHA-1 `e50e6ae5019b7b5bbf9a17837d3c814f8defae22`,
  `TU_16L61US_0000004000000.00000000001G1`, 2,453,504 bytes ("Crackdown Title Update #6").
- **DLC (optional):** kind `stfs`, `raw: true`: `AA10729E62721B4EAF674486684E31CD9B94AC4F4D` (Free-For-All Pack,
  92,962,816 bytes), `E4DDBC100F97B6954B84E82ED807E79B5A4314C04D` (Four-Play Pack, 9,089,024 bytes).

Crackdown 2 (new, optional):
- **Game:** the disc image (kind `file`, ext `iso`), or a folder of the disc's files with `default.xex`.
- **Title Update 5 (required for Crackdown 2):** kind `stfs`, `raw: true`, `to: "install"`: title ID `4D5308BC`,
  content ID `bf784b384e891bb57f2cc03498c5099abc148430`, SHA-1 `7c76d009d25e55f60996358904b96d524a856a52`,
  `TU_16L625S_0000004000000.0000000000181`, 3,317,760 bytes ("Crackdown 2 Title Update #5").
- **DLC (optional, multiple):** kind `stfs`, `raw: true`:
  - `856014F705032E789E4A6DBD2546F470B06ED71F4D` (Toy Box Pack, 51,814,400 bytes)
  - `BD0C27EFE61BEE10F176F3A624F8A851224A89694D` (Deluge Pack, 174,309,376 bytes)
  - `ACE4E65BC7BA561F3A27C1A5390B5904CB6C73EB4D` (Pre-Order Armor, 57,344 bytes)

Crackdown 3 (new, optional; not a store download): the UWPDumper folder of the player's own game (the one with
`AppxManifest.xml`) and the "Crackdown 3 Windows files" folder from `pc-helper/cd3_windows_files.py`, both copied into
`install/`; Prospero Win must be installed. On HOME, Crackdown 3's X installs them, then signs in to Xbox once.
