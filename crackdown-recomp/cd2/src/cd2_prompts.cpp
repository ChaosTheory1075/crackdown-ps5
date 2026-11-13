// Crackdown 2's PlayStation button prompts (the menu's BUTTONS row, crackdown_ps_buttons): see cd2_prompts_core.h.
//
// Before the title starts, game2/UI's ui.pack, pause.pack and initial.pack are copied into
// UserData/crackdown2/4D5308BC/prompts/UI with their button shapes replaced (/app0/glyphs/cd2_prompts.bin), and
// the game's UI\<pack> paths are pointed at the copies, as Crackdown 1's cd_glyphs.cpp points its font: the game
// files stay as they are. A copy is built again only when its inputs change (a stamp of their sizes and the rules').
#include "cd2_prompts.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>

#include "cd2_prompts_core.h"

REXCVAR_DEFINE_BOOL(crackdown_ps_buttons, true, "Crackdown", "PlayStation button prompts (Crackdown 2: cd2_prompts.cpp)");

namespace cd2::prompts {
namespace {

namespace fs = std::filesystem;

constexpr const char* kRules = "/app0/glyphs/cd2_prompts.bin";
constexpr const char* kMountPath = "\\Device\\CD2Prompts";
constexpr const char* kGamePartition = "\\Device\\Harddisk0\\Partition1";
constexpr const char* kPacks[] = {"ui", "pause", "initial"};
constexpr int kVersion = 1;

std::optional<std::vector<uint8_t>> ReadFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return std::nullopt;
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

bool WriteFile(const fs::path& p, const std::vector<uint8_t>& d) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  const fs::path tmp = p.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
    if (!f) return false;
  }
  fs::rename(tmp, p, ec);
  return !ec;
}

// The game's UI folder: game2/UI (the installer writes it as the disc has it).
fs::path GameUi(const fs::path& game_root) {
  std::error_code ec;
  for (const char* name : {"UI", "ui"})
    if (fs::is_directory(game_root / name, ec)) return game_root / name;
  return {};
}

}  // namespace

void Install(rex::filesystem::VirtualFileSystem* vfs, const fs::path& game_root, const fs::path& user_data_root) {
  if (!REXCVAR_GET(crackdown_ps_buttons)) {
    REXLOG_INFO("Crackdown 2 PS buttons: off (the game's Xbox 360 buttons)");
    return;
  }
  const auto blob = ReadFile(kRules);
  std::vector<Rule> rules;
  const fs::path ui = GameUi(game_root);
  if (!vfs || !blob || !ParseRules(*blob, rules) || ui.empty()) {
    REXLOG_WARN("Crackdown 2 PS buttons: the game's own buttons kept ({} or {}\\UI missing)", kRules, game_root.string());
    return;
  }
  const fs::path cache = user_data_root / "4D5308BC" / "prompts";
  const uint64_t rules_hash = ShapeHash(0, std::span<const uint8_t>(*blob));
  int linked = 0, shapes_total = 0;
  for (const char* name : kPacks) {
    const fs::path pack = ui / (std::string(name) + ".pack"), toc = ui / (std::string(name) + ".pack.toc");
    std::error_code ec;
    const auto pack_size = fs::file_size(pack, ec), toc_size = ec ? 0 : fs::file_size(toc, ec);
    if (ec) {
      REXLOG_WARN("Crackdown 2 PS buttons: {} missing, left as it is", pack.string());
      continue;
    }
    char stamp_text[96];
    std::snprintf(stamp_text, sizeof(stamp_text), "%d %llu %llu %016llx", kVersion, (unsigned long long)pack_size,
                  (unsigned long long)toc_size, (unsigned long long)rules_hash);
    const fs::path out_dir = cache / "UI";
    const fs::path out_pack = out_dir / (std::string(name) + ".pack"), out_toc = out_dir / (std::string(name) + ".pack.toc");
    const fs::path stamp_file = out_dir / (std::string(name) + ".stamp");
    const auto stamp = ReadFile(stamp_file);
    const bool fresh = stamp && std::string(stamp->begin(), stamp->end()) == stamp_text && fs::exists(out_pack, ec) &&
                       fs::exists(out_toc, ec);
    if (!fresh) {
      const auto pack_bytes = ReadFile(pack), toc_bytes = ReadFile(toc);
      PackResult r;
      std::string error;
      if (!pack_bytes || !toc_bytes || !PatchPack(*toc_bytes, *pack_bytes, rules, r, error)) {
        REXLOG_WARN("Crackdown 2 PS buttons: {} not patched ({})", pack.string(), error);
        continue;
      }
      if (!r.shapes) {
        REXLOG_WARN("Crackdown 2 PS buttons: no button shapes found in {} (another version of the game?)", pack.string());
        continue;
      }
      if (!WriteFile(out_pack, r.pack) || !WriteFile(out_toc, r.toc) ||
          !WriteFile(stamp_file, std::vector<uint8_t>(stamp_text, stamp_text + std::strlen(stamp_text)))) {
        REXLOG_WARN("Crackdown 2 PS buttons: cannot write {}", out_dir.string());
        continue;
      }
      REXLOG_INFO("Crackdown 2 PS buttons: {}.pack built, {} pieces in {} movies", name, r.shapes, r.movies);
      shapes_total += r.shapes;
    }
    ++linked;
  }
  if (!linked) return;
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(kMountPath, cache, true);
  if (!device->Initialize() || !vfs->RegisterDevice(std::move(device))) {
    REXLOG_WARN("Crackdown 2 PS buttons: could not mount {}", cache.string());
    return;
  }
  for (const char* name : kPacks) {
    std::error_code ec;
    if (!fs::exists(cache / "UI" / (std::string(name) + ".pack"), ec)) continue;
    // (one link covers the .toc too: the rest of the path is kept)
    vfs->RegisterSymbolicLink(std::string(kGamePartition) + "\\UI\\" + name + ".pack",
                              std::string(kMountPath) + "\\UI\\" + name + ".pack");
  }
  const std::string probe = "game:\\UI\\ui.pack.toc";
  if (rex::filesystem::Entry* entry = vfs->ResolvePath(probe))
    REXLOG_INFO("PlayStation button prompts, {} packs ({} pieces built now): {} is {}", linked, shapes_total, probe,
                entry->absolute_path());
  else
    REXLOG_WARN("Crackdown 2 PS buttons: {} does not resolve", probe);
}

}  // namespace cd2::prompts
