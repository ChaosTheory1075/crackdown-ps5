Put Crackdown's files here (this is PPSA53070/install/), then open the app:
it installs them by itself and shows the progress on its HOME page.

- The game: your disc image (.iso, any name), or a folder with the disc's
  files (default.xex and the rest).
- Title Update 6 (required): its package file, TU_16L61US_0000004000000.00000000001G1
  (any name). Other title updates (TU5, ...) can't be used.
- DLC (optional): the Free-For-All Pack and Four-Play Pack package files
  (the files named AA10729E... and E4DDBC10...).
- Saves (optional): Xbox 360 save packages; import them from the SAVES page.

Archives (.7z, .rar, .zip) aren't opened on the PS5: extract them on a PC
first (7-Zip), and copy what's inside here. Once everything is installed,
the app asks whether to delete the disc image and packages from here
(X deletes them, O keeps them); they aren't needed to play.

The app's CONTENT page shows what's installed and what's waiting here, and
installs (X) or uninstalls (triangle) each part. Backups made on the SAVES
page go to install/backups/.

Speed: the install is written by the app's installer payload
(payloads/crackdown-ps5-installer.elf), which it hands to your payload loader
(etaHEN or elfldr, port 9021), so it runs at the drive's full speed (a few
minutes for the game). With no payload loader running, the app copies by
itself, which the PS5 slows to a crawl after the first gigabyte.
