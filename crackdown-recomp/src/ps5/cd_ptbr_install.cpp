// Crackdown PS5 - Crackdown's text in Brazilian Portuguese, installed by the app (see cd_ptbr_install.h).
#include "cd_ptbr_install.h"

#if REX_PLATFORM_PS5

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#include <rex/logging.h>

// ptbr.bin (Recomps/ptbr/crackdown/pack.py): "CDPT", u32 LE count, count x (char path[56], u32 offset, u32
// size), the data. Paths are relative to game/: "Local/pt-BR/..." and "update/Local/pt-BR/...".
#define CD_PTBR_INCBIN(name, file)                                                      \
  __asm__(".section .rodata\n.global " #name "\n.balign 16\n" #name ":\n.incbin \"" \
          file "\"\n.global " #name "_end\n" #name "_end:\n.byte 0\n.previous\n");    \
  extern "C" const unsigned char name[];                                              \
  extern "C" const unsigned char name##_end[];
CD_PTBR_INCBIN(cdps5_ptbr, CDPS5_PTBR_BIN)

namespace cdps5 {
namespace {

namespace fs = std::filesystem;

struct Entry {
  std::string path;  // (relative to game/)
  const unsigned char* data;
  uint32_t size;
};

std::string Upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// The blob's entries; empty when this build has none (or it's damaged).
std::vector<Entry> Entries() {
  std::vector<Entry> out;
  const size_t n = size_t(cdps5_ptbr_end - cdps5_ptbr);
  uint32_t count = 0;
  if (n < 8 || std::memcmp(cdps5_ptbr, "CDPT", 4) != 0) return out;
  std::memcpy(&count, cdps5_ptbr + 4, 4);
  if (8 + size_t(count) * 64 > n) return out;
  for (uint32_t i = 0; i < count; ++i) {
    const unsigned char* e = cdps5_ptbr + 8 + size_t(i) * 64;
    char path[57] = {};
    std::memcpy(path, e, 56);
    uint32_t at = 0, size = 0;
    std::memcpy(&at, e + 56, 4);
    std::memcpy(&size, e + 60, 4);
    if (size_t(at) + size > n || !path[0] || std::strstr(path, "..")) return {};
    out.push_back({path, cdps5_ptbr + at, size});
  }
  return out;
}

// <dir>'s child named <name> in any case, or an empty path.
fs::path Child(const fs::path& dir, const std::string& name) {
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    if (Upper(it->path().filename().string()) == Upper(name)) return it->path();
  return {};
}

bool Write(const fs::path& path, const unsigned char* data, size_t size) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::FILE* f = std::fopen(path.string().c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(data, 1, size, f) == size;
  return std::fclose(f) == 0 && ok;
}

// One pt-BR folder: <local>/pt-BR from <local>/en-US plus the blob's files under <prefix> ("Local/pt-BR/"),
// written as pt-BR.installing and moved in when complete. Returns how many blob files went in (-1: failed).
int InstallOne(const fs::path& local, const std::vector<Entry>& entries, const std::string& prefix) {
  std::error_code ec;
  const fs::path en = Child(local, "en-US");
  if (en.empty()) {
    REXLOG_WARN("Crackdown Portuguese: no en-US folder in {}: its pt-BR folder not written", local.string());
    return -1;
  }
  const fs::path tmp = local / "pt-BR.installing";
  fs::remove_all(tmp, ec);
  fs::copy(en, tmp, fs::copy_options::recursive, ec);
  if (ec) {
    REXLOG_WARN("Crackdown Portuguese: copying {} failed: {}", en.string(), ec.message());
    fs::remove_all(tmp, ec);
    return -1;
  }
  int n = 0;
  for (const Entry& e : entries) {
    if (e.path.compare(0, prefix.size(), prefix) != 0) continue;
    if (!Write(tmp / e.path.substr(prefix.size()), e.data, e.size)) {
      REXLOG_WARN("Crackdown Portuguese: writing {} failed", (tmp / e.path.substr(prefix.size())).string());
      fs::remove_all(tmp, ec);
      return -1;
    }
    ++n;
  }
  fs::rename(tmp, local / "pt-BR", ec);
  if (ec) {
    REXLOG_WARN("Crackdown Portuguese: moving {} in failed: {}", tmp.string(), ec.message());
    fs::remove_all(tmp, ec);
    return -1;
  }
  return n;
}

}  // namespace

void InstallPortugueseText(const fs::path& app) {
  const std::vector<Entry> entries = Entries();
  if (entries.empty()) {
    REXLOG_INFO("Crackdown Portuguese: this build has no Portuguese text (English only)");
    return;
  }
  std::error_code ec;
  const fs::path game = app / "game";
  struct Folder {
    fs::path local;
    const char* prefix;
  };
  const fs::path update = Child(game, "update");
  const Folder folders[] = {{Child(game, "Local"), "Local/pt-BR/"},
                            {update.empty() ? fs::path() : Child(update, "Local"), "update/Local/pt-BR/"}};
  for (const Folder& f : folders) {
    if (f.local.empty()) continue;  // (the game, or Title Update 6, isn't installed yet)
    fs::remove_all(f.local / "pt-BR.installing", ec);  // (one the app was closed during)
    if (!Child(f.local, "pt-BR").empty()) {
      REXLOG_INFO("Crackdown Portuguese: {} is there already", (f.local / "pt-BR").string());
      continue;
    }
    const int n = InstallOne(f.local, entries, f.prefix);
    if (n >= 0) REXLOG_INFO("Crackdown Portuguese: wrote {} ({} Portuguese files)", (f.local / "pt-BR").string(), n);
  }

  // Crackdown 2 (game2/): the blob's "cd2/" files written straight into game2/Local/pt-BR and
  // game2/update/Local/pt-BR - no copy of en-US (the framework's CdLocalePath reads any file a pt-BR folder lacks
  // from en-US). A file is written when it's missing or a different size (a newer translation replaces an older).
  const fs::path game2 = app / "game2";
  const fs::path local2 = Child(game2, "Local"), update2 = Child(game2, "update");
  if (!local2.empty()) {
    int written = 0, same = 0;
    for (const Entry& e : entries) {
      if (e.path.compare(0, 4, "cd2/") != 0) continue;
      const std::string rel = e.path.substr(4);
      fs::path dst;
      if (rel.compare(0, 6, "Local/") == 0) {
        dst = local2 / rel.substr(6);
      } else if (rel.compare(0, 13, "update/Local/") == 0) {
        if (update2.empty()) continue;  // (Title Update 5 isn't installed)
        const fs::path ul = Child(update2, "Local");
        dst = (ul.empty() ? update2 / "Local" : ul) / rel.substr(13);
      } else {
        continue;
      }
      std::error_code ec2;
      if (fs::exists(dst, ec2) && fs::file_size(dst, ec2) == e.size) {
        ++same;
        continue;
      }
      if (Write(dst, e.data, e.size)) {
        ++written;
      } else {
        REXLOG_WARN("Crackdown 2 Portuguese: writing {} failed", dst.string());
      }
    }
    REXLOG_INFO("Crackdown 2 Portuguese: wrote {} files, {} already there (game2/Local/pt-BR)", written, same);
  }
}

}  // namespace cdps5

#endif  // REX_PLATFORM_PS5
