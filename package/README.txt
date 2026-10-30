Crackdown: ReActivated
======================

Crackdown (Xbox 360, Title Update 6) recompiled to run natively on the PS5,
with its DLC, your saves and its achievements as trophies. An unofficial fan
project: no game files are included, you need your own copy of Crackdown.

What you need
-------------
- A PS5 that runs homebrew apps from /data/homebrew/ or /mnt/ext1/homebrew/.
- Your Crackdown disc image (.iso) or the disc's files.
- Title Update 6's package (TU_16L61US_0000004000000.00000000001G1).
- Optional: the Free-For-All Pack and Four-Play Pack DLC.

Install
-------
1. Copy this whole PPSA53070 folder to the PS5, wherever your homebrew
   loader looks: /data/homebrew/, /mnt/ext0-3/homebrew/, /mnt/usb0-4/homebrew/
   and so on. The folder can be renamed; the app finds its own files.
2. Put these in PPSA53070/install/ (see install/README.txt):
   - your Crackdown disc image (.iso), or a folder of the disc's files,
   - Title Update 6's package (required),
   - the DLC packages (optional).
   Extract any .7z / .rar / .zip on a PC first.
3. Open "Crackdown: ReActivated". It installs what it finds, with the progress
   on its HOME page (the 7.8 GB disc image takes a few minutes). When PLAY
   lights up, you're set; the CONTENT page shows what's installed.

Already have the game's files extracted? Put them in PPSA53070/install/ as a
folder, or straight in PPSA53070/game/ (with Title Update 6's Default.xexp
next to default.xex).

The menu
--------
HOME      PLAY; an install's progress; what's missing and where it goes.
CONTENT   the game, Title Update 6 and the two DLC packs: installed or not,
          X installs one from install/, triangle (twice) uninstalls it.
SAVES     BACK UP SAVES (to install/backups/), IMPORT an Xbox 360 save
          package from install/, RESTORE one of the backups.
SETTINGS  LANGUAGE          AUTO (the PS5's language) or any of the 13 on
                            the disc.
          FRAME RATE        60 (as fast as the PS5 runs it) or 30 locked (steady).
          ANTI-ALIASING     off or FXAA.
          TEXTURE FILTERING 2x to 16x anisotropic.
          GPU HELPER        a second graphics thread (on); turn it off if
                            something draws wrong.
          GPU MEMORY REFRESH  off; turn it on only if something looks stuck
                            or out of date (much slower).
          MENU MUSIC        the menu song's level.
TROPHIES  every achievement, earned ones first, with their dates (filled in
          once the game has been started once).
ABOUT     the credits.
L1 / R1 change pages, O goes back to HOME. Settings are kept in menu.ini.

Your data (back these up before deleting or reinstalling the app!)
------------------------------------------------------------------
- PPSA53070/saves/          your game saves
- PPSA53070/UserData/       trophies (earned and their dates) and the game's cache
- PPSA53070/menu.ini        your settings
SAVES > BACK UP SAVES copies all three to PPSA53070/install/backups/: copy
that folder off the PS5. Deleting the PPSA53070 folder deletes them too.

Updating to a new build
-----------------------
Replace eboot.bin, payloads/, sce_sys/ and sce_module/ (and hub/ if it changed). Keep game/,
dlc/, install/, saves/, UserData/ and menu.ini.

Problems
--------
- PLAY stays grey: the HOME and CONTENT pages say what's missing.
- An install failed: the CONTENT page says why (not enough space, not the
  right file, ...). Fix it and press X on that row.
- Something draws wrong: SETTINGS > GPU HELPER off, then GPU MEMORY REFRESH on.
- The home screen tile, background or music didn't change after an update:
  restart the PS5 (it caches them).
- A log of every run is written to PPSA53070/logs/. Send the newest one with
  any report.

Credits
-------
Crackdown: ReActivated by ChaosTheory1075 (github.com/ChaosTheory1075/crackdown-ps5).

Built on elripalda's GTA IV for PS5 (github.com/elripalda/gta4-ps5), its
installer payload included; saulob's Crackdown Recompiled
(github.com/saulob/crackdown-xbox360-recompiled); OZORDI's Liberty Recompiled
and vaduur's LibertyRecompAndroid; ReXGlue and Xenia.
Hover music made with BlackBearReloaded's PS5 AT9 Converter.
Special thanks to Realtime Worlds, for making Crackdown.

Crackdown and all related names belong to their owners. This project is not
affiliated with or endorsed by Microsoft, Realtime Worlds or Sony.
