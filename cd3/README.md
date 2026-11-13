# Crackdown 3: the Wine side and the PC helper

Crackdown 3 is the PC (Microsoft Store / Game Pass) version, run on the PS5 by
[Prospero Win](https://github.com/mpereiraesaa/prospero-win) with a Wine built for UWP apps. The app's
`cd3data/overlay` holds that Wine's DLLs (LGPL-2.1-or-later, like Wine); nothing in the app is Microsoft's.

## `wine-source/`: the source of those Wine DLLs
- `pw-merge-patches/`: the full patch series (122 patches, `git format-patch`) from the
  [designgears/wine-uwp](https://github.com/designgears/wine-uwp) base: Prospero Win's own Wine patches (its
  `wine/ps5` series, commit 18115ce) plus this project's (the PS5 address space, preloads, Xbox achievement
  events, Gamepad over XInput, ...). Apply them in order with `git am` to that base.
- `wine-uwp-src/`: the files this project changed in the wine-uwp tree itself, complete: `winexboxauth` (Xbox
  sign-in, cloud saves without freezing the game, achievements), `windows.gaming.input` (the DualSense as an Xbox
  Gamepad; the whole touch pad opens the map), `ntdll/misc.c`, `sechost/service.c`. Lines changed for Crackdown 3
  are marked `[cd3]`.

## `pc-helper/`: what the player runs on a PC
- `cd3_windows_files.py`: takes the Windows 11 files Crackdown 3 needs out of Microsoft's own Windows 11
  evaluation ISO (downloaded from Microsoft, or one you have), checks each against `ms-files.tsv`, and makes the
  "Crackdown 3 Windows files" folder you copy into the app's `install/`. Needs Python 3.8+ and 7-Zip.
- `make_cd3_template.py`, `make_cd3_overlay.sh`, `regedit_lines.py`, `winexboxauth_import.py`: how the app's
  `cd3data` (the Wine prefix template and the DLL overlay) was made.

## PlayStation button prompts
`ps5/payload/installer/cd3_prompts.inc` builds `Nimbus-UWP_P.pak` on the console from the player's own
`Nimbus-UWP.pak`, with our own drawn glyphs (`cd3data/ps-prompts.bin`: pixel data and offsets only, no game data).
