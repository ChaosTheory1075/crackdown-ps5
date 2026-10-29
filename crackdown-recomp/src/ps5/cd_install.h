// Crackdown PS5 - the installer the menu's CONTENT and SAVES pages run: what's
// in the app's install folder (<app>/install), what's installed, and the jobs
// that install, uninstall, import and back up, on a thread of their own.
//
//  - The game: a disc image (.iso, any name) or a folder of the disc's files
//    (anywhere in install/), into <app>/game.
//  - Title Update 6 (its package, any name): Default.xexp into game/ (as both
//    default.xexp and Default.xexp), its other files into game/update/. Other
//    title updates are named but not installed: the program is TU6's.
//  - The DLC packages: each into <app>/dlc/<its package name>/.
//  - Saves: Xbox 360 save packages, into <app>/saves the way the runtime keeps
//    them; and the app's own backups (install/backups/...), restored.
//
// Everything is found from what it is (an image's file system, a package's
// header), never from its name. An install is written next to its place and
// moved in when complete, so an interrupted one leaves nothing half done. The
// files installed from stay until the player chooses to delete them (the menu
// asks once the installs are done). Archives (.7z, .rar, .zip) are listed to be
// extracted on a PC first.
#pragma once

#include <rex/platform.h>
#if REX_PLATFORM_PS5

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace cdps5::install {

namespace fs = std::filesystem;

constexpr int kDlcCount = 2;
// "FREE-FOR-ALL PACK", "FOUR-PLAY PACK"
const char* DlcName(int dlc);

enum class Kind {
  kGameImage,    // a disc image
  kGameFolder,   // a folder with default.xex
  kUpdate,       // Title Update 6
  kOtherUpdate,  // another title update (TU5, ...): not installable
  kDlc,
  kSave,         // an Xbox 360 save package
  kBackup,       // one of this app's backups
  kArchive,      // .7z / .rar / .zip: extract on a PC
  kOther,        // another game's package, or not known
};

struct Source {
  Kind kind = Kind::kOther;
  fs::path path;
  std::string label;  // for the menu
  int dlc = -1;       // (kDlc) which
};

struct Installed {
  bool game = false, update = false;
  bool dlc[kDlcCount] = {};
  int saves = 0;
};

fs::path Folder(const fs::path& app);  // <app>/install (made if missing)
// Where the install folder really is, for the screen ("/mnt/usb0/homebrew/PPSA53070/install"):
// the app's folder can be on any drive, under any name.
std::string Where(const fs::path& app);
Installed Check(const fs::path& app);
std::vector<Source> Scan(const fs::path& app);
// The ones installed without being asked, as the menu opens: the game, TU6, the DLC.
bool Automatic(Kind kind);

enum class Remove { kGame, kUpdate, kDlc0, kDlc1 };

// One job at a time, on its own thread.
class Job {
 public:
  bool Busy() const { return busy_.load(std::memory_order_acquire); }
  // (0..1; with Busy)
  float Progress() const;
  std::string Title() const;
  // The last finished job's outcome ("" none yet) and whether it failed.
  std::string Result() const;
  bool Failed() const { return failed_.load(std::memory_order_acquire); }
  // Bumped when a job finishes (the menu looks at the folders again).
  uint32_t Generation() const { return generation_.load(std::memory_order_acquire); }

  bool Install(const fs::path& app, const Source& source);
  bool Uninstall(const fs::path& app, Remove what);
  bool Backup(const fs::path& app);

  // The files installed from this session (disc images, packages), still in the install folder:
  // the menu asks whether to delete them.
  std::vector<fs::path> Leftovers() const;
  void ForgetLeftovers();  // (kept)
  bool DeleteLeftovers();  // (a job)

 private:
  template <typename F>
  bool Start(std::string title, F&& work);
  void Keep(const fs::path& source);

  std::atomic<bool> busy_{false}, failed_{false};
  std::atomic<uint32_t> generation_{0};
  std::atomic<uint64_t> done_{0}, total_{0};
  mutable std::mutex mutex_;
  std::string title_, result_;
  std::vector<fs::path> leftovers_;
};

Job& TheJob();

// As the game starts (the runtime's profile made): imported saves (saves/.import) moved into the
// profile's folder, saves/<xuid>/, replacing one of the same name.
void AdoptImportedSaves(const fs::path& saves, uint64_t xuid);

}  // namespace cdps5::install

#endif  // REX_PLATFORM_PS5
