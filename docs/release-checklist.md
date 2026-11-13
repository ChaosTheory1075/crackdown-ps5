# Crackdown: ReActivated 1.0.0: release checklist

## Tested on the PS5 (2026-10-29)
- [x] Install through the payload: game, TU6, DLC (both packs)
- [x] Delete question after installing: X deletes, O keeps, kept files aren't installed again
- [x] Save import: loads the save in game, trophies unlock from it
- [x] 60 FPS, trophies, menu music, art

## Tonight, before going public
- [ ] Fresh install from the release zip itself: `Downloads\Crackdown\Extras\release\Crackdown-ReActivated-1.0.0.zip`
  - unzip, copy `PPSA53070` to the PS5, put the ISO, TU6 and DLC in `install/`, open it
  - ideally somewhere other than `/data/homebrew` (a USB drive, or the folder renamed)
- [ ] SAVES page: BACK UP SAVES, then RESTORE that backup
- [ ] (optional) open the app with no payload loader running: it should still install, slowly

## Publish (one go-ahead covers these)
- [x] Rename the private GitHub repo `crackdown-ps5` to `crackdown-ps5-dev` (its history has the encrypted game files)
- [x] Create a public `ChaosTheory1075/crackdown-ps5` and push the clean repo from `Downloads\MP3\crackdown-ps5-public`
  (one commit, GitHub noreply email, no game files)
- [x] GitHub release `1.0.0` with `Crackdown-ReActivated-1.0.0.zip` attached
- [ ] Pin the repo on the GitHub profile

## RecompStore (recompstore.com/submit; the text is in docs/recompstore-submission.md)
- [ ] Optional: create a free developer account first (it fills the form from GitHub and gives a verified page)
- [ ] Pictures:
  - [x] Icon, square, 512x512 or bigger: `package/app-assets/sce_sys/icon0.png` (512x512)
  - [x] Backdrop, 16:9, ideally 3840x2160: `hub/background.jpg` is 1920x1200 (16:10). Crop to 16:9 or make a 4K one
  - [x] Logo, the title on a transparent background: the emblem with REACTIVATED. Only the REACTIVATED wordmark exists as a transparent PNG (`hub/logo.png`), so the full logo needs making
  - [x] Screenshots, up to 4, taken on the PS5 (gameplay, the menu's HOME, CONTENT and TROPHIES pages)
- [ ] Paste the fields: name, made by, repo, release file `Crackdown-ReActivated-*.zip`, version 1.1.0, title ID PPSA53070, summary, description, what players need, credits, links
- [ ] "Anything else?": the store setup. Everything is copied as is into `install/`; TU6 is downloaded with content ID `65eda8ad841eb6a5b03f94807888ff026b5577ff` and SHA-1 `e50e6ae5019b7b5bbf9a17837d3c814f8defae22`
- [ ] Tick the box and send it

## Later
- [ ] License: ask saulob about GPL-2.0-or-later (or written permission) for Crackdown Recompiled
- [ ] Teaser follow-ups: a release-date version, a launch trailer

## 1.0.1 (2026-10-30)
- [x] LANGUAGE setting: all 13 of the disc's text folders (TU6 adds none), AUTO follows the PS5
- [x] Public repo: commit be5f28f (also adds the installer payload's rex headers, left out of 1.0.0)
- [x] GitHub release v1.0.1 with `Crackdown-ReActivated-1.0.1.zip` (SHA-256 9791d7188dc2bac10c9e291110e6b934de9699bd694f53bf5acf2d84af16c32b)
- [ ] On the PS5: Polish, Czech, Hungarian, Slovak and Latin American Spanish (this build picks the text folder itself; the tested build only set the console language)

## 1.1.0 (2026-10-31)
- [x] HUD message panels and subtitles: the guest is told 1280x720, not the PS5's 4K display (f03eb57)
- [x] Cheats + quick menu (L3+R3), PS button prompts, DualSense, FPS counter, skip intro, Jump Height on TU6
- [x] PS5 log of the release build's eboot (crackdown_023): no errors, city 32-58 fps
- [x] Public repo: commit 7350a79
- [x] GitHub release v1.1.0 with `Crackdown-ReActivated-1.1.0.zip` (SHA-256 5ac9569ac670bb97d7764638ded0d78b795a2038796b1f181952316d388dc77d)
- [ ] On the PS5: the HUD messages and subtitles seen in game; a fresh install from the 1.1.0 zip
