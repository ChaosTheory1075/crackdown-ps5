// Crackdown PS5 - PlayStation button prompts (see cd_glyphs.h).
#include "cd_glyphs.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>

#include "../font_atlas.h"

namespace fs = std::filesystem;
namespace fa = crackdown::font_atlas;

namespace cdps5::glyphs {
namespace {

// Bumped when the built font changes for the same inputs, so the copies are built again.
constexpr std::string_view kVersion = "ps-buttons-1";
constexpr const char* kSheet = "/app0/glyphs/ps_button_glyphs.dds";
constexpr const char* kCrossPicture = "/app0/glyphs/ps_cross_40.png";
constexpr const char* kCirclePicture = "/app0/glyphs/ps_circle_40.png";
constexpr uint32_t kAtlasScale = 2;
constexpr uint32_t kCell = 128, kColumns = 8;
constexpr char16_t kFirstButton = 0x0190;
constexpr uint32_t kButtons = 40;
constexpr std::string_view kMountPath = "\\Device\\CrackdownGlyphs";
constexpr std::string_view kGamePartition = "\\Device\\Harddisk0\\Partition1";
constexpr const char* kFontFiles[] = {"Crackdown_Font.abc", "Crackdown_Font.dds"};

std::optional<std::vector<uint8_t>> ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

bool WriteFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  return bool(out);
}

// FNV-1a over the inputs: the copy is rebuilt when any of them changes.
struct Stamp {
  uint64_t h = 1469598103934665603ull;
  void Add(const void* data, size_t size) {
    for (size_t i = 0; i < size; ++i) h = (h ^ static_cast<const uint8_t*>(data)[i]) * 1099511628211ull;
  }
  std::string Text() const {
    char text[24];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(h));
    return text;
  }
};

// Whether the sheet has art in cell n (an empty one keeps the game's glyph).
bool HasArt(const fa::Image& sheet, const fa::Rect& cell) {
  for (uint32_t y = cell.y0; y < cell.y1; ++y)
    for (uint32_t x = cell.x0; x < cell.x1; ++x)
      if (sheet.rgba[(size_t(y) * sheet.width + x) * 4 + 3]) return true;
  return false;
}

// The language's font, built into `folder`.
bool BuildFont(const std::vector<uint8_t>& abc, const std::vector<uint8_t>& dds, const fa::Image& sheet,
               const fs::path& folder) {
  auto metrics = fa::ParseAbc(abc);
  auto atlas = fa::ReadDds(dds);
  if (!metrics || !atlas || !fa::GlyphRectsFit(*metrics, atlas->width, atlas->height)) {
    REXLOG_WARN("PS buttons: the game's font could not be read");
    return false;
  }
  fa::Image image = fa::Upscale(*atlas, kAtlasScale);
  if (image.width == 0 || !fa::ScaleGlyphRects(*metrics, kAtlasScale)) return false;
  uint32_t replaced = 0;
  for (uint32_t n = 0; n < kButtons; ++n) {
    const char16_t c = char16_t(kFirstButton + n);
    if (c > metrics->max_char || metrics->map[c] == 0) {
      REXLOG_WARN("PS buttons: the font has no button glyph U+{:04X}", uint32_t(c));
      return false;
    }
    const fa::Rect cell{(n % kColumns) * kCell, (n / kColumns) * kCell, (n % kColumns + 1) * kCell,
                        (n / kColumns + 1) * kCell};
    if (cell.y1 > sheet.height || !HasArt(sheet, cell)) continue;
    if (fa::PasteFit(image, fa::GlyphRect(metrics->glyphs[metrics->map[c]]), sheet, cell)) ++replaced;
  }
  std::error_code ec;
  fs::create_directories(folder, ec);
  if (ec || !WriteFile(folder / kFontFiles[0], fa::WriteAbc(*metrics)) ||
      !WriteFile(folder / kFontFiles[1], fa::WriteDdsBc3(image, true))) {
    REXLOG_WARN("PS buttons: cannot write {}", folder.string());
    return false;
  }
  REXLOG_INFO("PS buttons: {} glyphs replaced, atlas {}x{}", replaced, image.width, image.height);
  return true;
}

// A file copied with plain reads and writes (std::filesystem::copy_file failed on the PS5:
// crackdown_017.log).
bool CopyFile(const fs::path& from, const fs::path& to) {
  std::ifstream in(from, std::ios::binary);
  std::ofstream out(to, std::ios::binary | std::ios::trunc);
  if (!in || !out) return false;
  std::vector<char> buffer(1 << 20);
  while (in) {
    in.read(buffer.data(), std::streamsize(buffer.size()));
    out.write(buffer.data(), in.gcount());
  }
  return bool(out);
}

// Every file of the language folder but the font, copied beside the built font (the whole folder is
// redirected), keeping copies whose size already matches. One folder level at a time.
bool MirrorLanguageFolder(const fs::path& source, const fs::path& folder, bool top = true) {
  std::error_code ec;
  fs::create_directories(folder, ec);
  for (auto it = fs::directory_iterator(source, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    const std::string name = it->path().filename().string();
    const fs::path target = folder / name;
    std::error_code e;
    if (it->is_directory(e)) {
      if (!MirrorLanguageFolder(it->path(), target, false)) return false;
      continue;
    }
    if (top && (name == kFontFiles[0] || name == kFontFiles[1])) continue;
    std::error_code size_ec;
    if (fs::file_size(target, size_ec) == it->file_size(e) && !size_ec) continue;
    if (!CopyFile(it->path(), target)) {
      REXLOG_WARN("PS buttons: cannot copy {} to {}", it->path().string(), target.string());
      return false;
    }
  }
  if (ec) REXLOG_WARN("PS buttons: cannot list {} ({})", source.string(), ec.message());
  return !ec;
}

// ---- the menus' A.png and B.png (XUI packages: "XUIZ", big-endian) ----
//
// Header: magic, u32 version 1, u32 file size, u32 0, u32 table size, u16 entries; then per entry u32 size,
// u32 offset (from the end of the table), u8 name length and the UTF-16BE name; then the data.

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void PutBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

// The package with its files named `leaf` (A.png, B.png) replaced; empty if it isn't one.
std::optional<std::vector<uint8_t>> ReplaceInPackage(const std::vector<uint8_t>& in,
                                                     const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
  if (in.size() < 22 || std::string_view(reinterpret_cast<const char*>(in.data()), 4) != "XUIZ") return std::nullopt;
  const uint32_t table = Be32(&in[16]);
  const uint32_t count = uint32_t(in[20]) << 8 | in[21];
  const size_t data_start = 22 + size_t(table);
  if (data_start > in.size()) return std::nullopt;
  struct Entry {
    size_t record;  // where its size and offset are
    std::string leaf;
    uint32_t size, offset;
  };
  std::vector<Entry> entries;
  size_t pos = 22;
  for (uint32_t i = 0; i < count; ++i) {
    if (pos + 9 > data_start) return std::nullopt;
    Entry e{pos, {}, Be32(&in[pos]), Be32(&in[pos + 4])};
    const size_t length = in[pos + 8];
    if (pos + 9 + 2 * length > data_start) return std::nullopt;
    std::string name;
    for (size_t k = 0; k < length; ++k) name += char(in[pos + 9 + 2 * k + 1]);  // (ASCII names)
    e.leaf = name.substr(name.find_last_of("\\/") + 1);
    if (data_start + size_t(e.offset) + e.size > in.size()) return std::nullopt;
    entries.push_back(e);
    pos += 9 + 2 * length;
  }
  if (pos != data_start) return std::nullopt;
  std::vector<uint8_t> out(in.begin(), in.begin() + data_start);
  for (const Entry& e : entries) {
    const std::vector<uint8_t>* replacement = nullptr;
    for (const auto& [leaf, bytes] : files)
      if (e.leaf == leaf) replacement = &bytes;
    const uint32_t offset = uint32_t(out.size() - data_start);
    if (replacement) {
      out.insert(out.end(), replacement->begin(), replacement->end());
    } else {
      out.insert(out.end(), in.begin() + data_start + e.offset, in.begin() + data_start + e.offset + e.size);
    }
    PutBe32(&out[e.record], replacement ? uint32_t(replacement->size()) : e.size);
    PutBe32(&out[e.record + 4], offset);
  }
  PutBe32(&out[8], uint32_t(out.size()));
  return out;
}

}  // namespace

void InstallFont(rex::filesystem::VirtualFileSystem* vfs, const fs::path& game_root, const fs::path& user_data_root,
                 std::string_view language) {
  const fs::path source = game_root / "Local" / std::string(language);
  const auto abc = ReadFile(source / kFontFiles[0]);
  const auto dds = ReadFile(source / kFontFiles[1]);
  const auto sheet_dds = ReadFile(kSheet);
  if (!vfs || !abc || !dds || !sheet_dds) {
    REXLOG_WARN("PS buttons: the game's own glyphs kept (font of {} or {} missing)", source.string(), kSheet);
    return;
  }
  Stamp stamp;
  stamp.Add(kVersion.data(), kVersion.size());
  for (const auto* bytes : {&*abc, &*dds, &*sheet_dds}) stamp.Add(bytes->data(), bytes->size());
  const fs::path cache_root = user_data_root / "4D5307DC" / "glyphs";
  const fs::path folder = cache_root / std::string(language);
  const auto cached = ReadFile(folder / "stamp");
  std::error_code ec;
  if (!cached || std::string(cached->begin(), cached->end()) != stamp.Text() ||
      !fs::exists(folder / kFontFiles[0], ec) || !fs::exists(folder / kFontFiles[1], ec)) {
    const auto sheet = fa::ReadDds(*sheet_dds);
    if (!sheet || !BuildFont(*abc, *dds, *sheet, folder)) {
      REXLOG_WARN("PS buttons: the game's own glyphs kept (the font could not be built)");
      return;
    }
    const std::string text = stamp.Text();
    WriteFile(folder / "stamp", std::vector<uint8_t>(text.begin(), text.end()));
    REXLOG_INFO("PS buttons: built the {} font in {}", language, folder.string());
  }
  if (!MirrorLanguageFolder(source, folder)) {
    REXLOG_WARN("PS buttons: the game's own glyphs kept (cannot copy {} into {})", source.string(), folder.string());
    return;
  }
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(std::string(kMountPath), cache_root, true);
  if (!device->Initialize() || !vfs->RegisterDevice(std::move(device))) {
    REXLOG_WARN("PS buttons: could not mount {}", cache_root.string());
    return;
  }
  vfs->RegisterSymbolicLink(std::string(kGamePartition) + "\\Local\\" + std::string(language),
                            std::string(kMountPath) + "\\" + std::string(language));
  const std::string probe = "game:\\Local\\" + std::string(language) + "\\Crackdown_Font.dds";
  if (rex::filesystem::Entry* entry = vfs->ResolvePath(probe))
    REXLOG_INFO("PS buttons: {} is {}", probe, entry->absolute_path());
  else
    REXLOG_WARN("PS buttons: {} does not resolve", probe);
}

void SetMenuPictures(const fs::path& game_root, bool playstation) {
  const auto cross = ReadFile(kCrossPicture);
  const auto circle = ReadFile(kCirclePicture);
  for (const char* name : {"agencyNode.xzp", "dossier.xzp"}) {
    const fs::path path = game_root / "XUIFrontEnd" / name;
    fs::path backup = path;
    backup += ".xbox";
    std::error_code ec;
    if (!playstation) {
      if (fs::exists(backup, ec)) {
        fs::rename(backup, path, ec);
        if (ec) REXLOG_WARN("PS buttons: cannot put back {} ({})", path.string(), ec.message());
      }
      continue;
    }
    if (!cross || !circle) return;
    // From the original each time (the backup once it exists), so a newer picture replaces an older one.
    const auto original = ReadFile(fs::exists(backup, ec) ? backup : path);
    if (!original) continue;
    const auto patched = ReplaceInPackage(*original, {{"A.png", *cross}, {"B.png", *circle}});
    if (!patched) {
      REXLOG_WARN("PS buttons: {} isn't a package this reads", path.string());
      continue;
    }
    const auto current = ReadFile(path);
    if (current && *current == *patched) continue;
    if (!fs::exists(backup, ec) && (fs::copy_file(path, backup, ec), ec)) {
      REXLOG_WARN("PS buttons: cannot keep {} ({})", backup.string(), ec.message());
      continue;
    }
    if (!WriteFile(path, *patched)) REXLOG_WARN("PS buttons: cannot write {}", path.string());
    else REXLOG_INFO("PS buttons: Cross and Circle in {}", path.string());
  }
}

}  // namespace cdps5::glyphs
