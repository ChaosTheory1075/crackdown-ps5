// Crackdown PS5 - Crackdown 3 on HOME: what its setup has, and its two jobs (install, Xbox sign-in).
//
// Crackdown 3 is the player's own PC (UWP) copy, run by Prospero Win (PPSA99995) through its "cd3" profile. The
// player puts the UWPDumper dump and the PC helper's "Crackdown 3 Windows files" in the app's install/ folder; the
// install (payloads/crackdown-ps5-cd3.elf, ps5/payload/installer/cd3.cpp) moves them into <app>/cd3, builds the
// prefix, the profile and Prospero Win's runtime overlay. The sign-in is Microsoft's device-code flow: the payload
// gets a code, this shows it (and a QR code of the link), and the payload leaves the token for Wine to take in.
#pragma once

#include <rex/platform.h>
#if REX_PLATFORM_PS5

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cdps5::cd3 {

namespace fs = std::filesystem;

// What's there (cheap: a few stats).
struct Setup {
  bool prospero = false;   // Prospero Win (PPSA99995) on any drive
  bool installed = false;  // <app>/cd3/installed.txt, the profile, the prefix and the game's exe
  bool signed_in = false;  // a sign-in in the prefix (xboxauth.bin, or one waiting to be taken in)
  bool dump = false;       // a dump in install/ (or the game already in <app>/cd3/game)
  bool windows = false;    // the Windows files in install/ (or in <app>/cd3/windows)
  bool data = false;       // the app's own Crackdown 3 files (<app>/cd3data)
  bool Playable() const { return prospero && installed; }
  static Setup Find(const fs::path& app);
};

// The payload job the app is following (one at a time).
struct Job {
  enum class Kind { kNone, kInstall, kSignIn, kLaunch };
  Kind kind = Kind::kNone;
  std::string phase;    // "", "working", "code", "done", "failed", "stopped"
  std::string message;  // what it's doing
  std::string error;    // (failed)
  std::string code;     // (sign-in) the code to type at microsoft.com/link
  std::string url;      // (sign-in) the link with the code in it, for the QR code
  uint64_t done = 0, total = 0;
  double started = 0;   // (seconds, the app's clock)
  bool Active() const { return kind != Kind::kNone && phase != "done" && phase != "failed" && phase != "stopped"; }
};

// Starts the install or the sign-in: the job file, and the payload sent to the payload loader (127.0.0.1:9021).
// false (with `error`): no payload loader, or the payload is missing from the app.
// extra: more "key=value" lines (each ending in a newline) for the payload (launch: "prompts=ps|xbox").
bool Start(const fs::path& app, Job::Kind kind, Job& job, std::string& error, const std::string& extra = "");
// Reads the payload's status (call a few times a second while Active()). A payload that never started or went
// silent for long is reported as failed.
void Tick(const fs::path& app, Job& job, double now);
// Asks the running job to stop.
void Stop(const fs::path& app);

// A QR code of `text`: size x size modules, row by row (true = dark). Empty when it can't be made.
std::vector<bool> QrModules(const std::string& text, int& size);

}  // namespace cdps5::cd3

#endif  // REX_PLATFORM_PS5
