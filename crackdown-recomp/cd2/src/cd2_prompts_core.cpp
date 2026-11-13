// Crackdown 2's PlayStation button prompts: the movie and pack rewriting (see cd2_prompts_core.h).
#include "cd2_prompts_core.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>

// stb_image's inflate, a private copy (the framework's and the menu's are static to their files)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#include "stb_image.h"

namespace cd2::prompts {
namespace {

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void PutLe16(std::vector<uint8_t>& o, uint32_t v) { o.push_back(uint8_t(v)), o.push_back(uint8_t(v >> 8)); }
void PutLe32(std::vector<uint8_t>& o, uint32_t v) { PutLe16(o, v & 0xFFFF), PutLe16(o, v >> 16); }
void SetLe32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8), p[2] = uint8_t(v >> 16), p[3] = uint8_t(v >> 24); }
void SetBe32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }

// The bits of a SWF RECT/MATRIX/shape record, most significant first.
struct BitWriter {
  std::vector<uint8_t> out;
  int used = 8;
  void Bits(uint32_t v, int n) {
    for (int i = n - 1; i >= 0; --i) {
      if (used == 8) out.push_back(0), used = 0;
      if ((v >> i) & 1) out.back() |= uint8_t(0x80 >> used);
      ++used;
    }
  }
  void Signed(int32_t v, int n) { Bits(uint32_t(v) & ((n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1)), n); }
  void Align() { used = 8; }
};

int SignedBits(int32_t v) {
  int n = 1;
  while (n < 32 && (v < -(1 << (n - 1)) || v >= (1 << (n - 1)))) ++n;
  return n;
}

struct Rect {
  int32_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
  size_t bytes = 0;  // its size in the tag
};

bool ReadRect(const uint8_t* p, size_t avail, Rect& r) {
  if (!avail) return false;
  const int n = p[0] >> 3;
  const size_t bits = 5 + 4 * size_t(n);
  r.bytes = (bits + 7) / 8;
  if (r.bytes > avail) return false;
  size_t bit = 5;
  auto get = [&]() {
    int32_t v = 0;
    for (int i = 0; i < n; ++i, ++bit) v = (v << 1) | ((p[bit >> 3] >> (7 - (bit & 7))) & 1);
    if (n && (v & (1 << (n - 1)))) v -= 1 << n;
    return v;
  };
  r.x0 = get(), r.x1 = get(), r.y0 = get(), r.y1 = get();
  return true;
}

void PutTag(std::vector<uint8_t>& o, uint16_t code, const std::vector<uint8_t>& body) {
  PutLe16(o, uint16_t(code << 6 | 0x3F));  // (the long form always: a length that never truncates)
  PutLe32(o, uint32_t(body.size()));
  o.insert(o.end(), body.begin(), body.end());
}

// DefineBitsLossless2 `bitmap_id` + DefineShape3 `shape_id`: a rectangle of `bounds` filled with the picture.
void PutReplacement(std::vector<uint8_t>& o, uint16_t shape_id, const uint8_t* rect_bytes, const Rect& bounds,
                    uint16_t bitmap_id, const Rule& rule) {
  std::vector<uint8_t> bits;
  PutLe16(bits, bitmap_id);
  bits.push_back(5);  // 32-bit ARGB
  PutLe16(bits, rule.width);
  PutLe16(bits, rule.height);
  bits.insert(bits.end(), rule.zlib_argb.begin(), rule.zlib_argb.end());
  PutTag(o, 36, bits);

  std::vector<uint8_t> s;
  PutLe16(s, shape_id);
  s.insert(s.end(), rect_bytes, rect_bytes + bounds.bytes);  // (the original bounds, as they were)
  s.push_back(1);     // one fill
  s.push_back(0x41);  // clipped bitmap, smoothed
  PutLe16(s, bitmap_id);
  {
    // bitmap pixels -> twips: scale to the bounds, moved to their corner (16.16 scale, twips translate)
    BitWriter m;
    const int32_t sx = int32_t(double(bounds.x1 - bounds.x0) / rule.width * 65536.0);
    const int32_t sy = int32_t(double(bounds.y1 - bounds.y0) / rule.height * 65536.0);
    const int ns = std::max(SignedBits(sx), SignedBits(sy));
    m.Bits(1, 1), m.Bits(uint32_t(ns), 5), m.Signed(sx, ns), m.Signed(sy, ns);
    m.Bits(0, 1);  // no rotate
    const int nt = std::max(SignedBits(bounds.x0), SignedBits(bounds.y0));
    m.Bits(uint32_t(nt), 5), m.Signed(bounds.x0, nt), m.Signed(bounds.y0, nt);
    s.insert(s.end(), m.out.begin(), m.out.end());
  }
  s.push_back(0);  // no line styles
  {
    BitWriter r;
    r.Bits(1, 4), r.Bits(0, 4);  // fill index bits 1, line index bits 0
    // style change: move to the top left corner, fill style 1 (the right side of a clockwise rectangle)
    const int nm = std::max(SignedBits(bounds.x0), SignedBits(bounds.y0));
    r.Bits(0, 1), r.Bits(0x01 | 0x04, 5);  // (StateMoveTo, StateFillStyle1)
    r.Bits(uint32_t(nm), 5), r.Signed(bounds.x0, nm), r.Signed(bounds.y0, nm);
    r.Bits(1, 1);  // fill style 1 = 1
    auto line = [&](int32_t dx, int32_t dy) {  // straight edge, horizontal or vertical
      const int n = std::max(2, SignedBits(dx ? dx : dy));
      r.Bits(1, 1), r.Bits(1, 1), r.Bits(uint32_t(n - 2), 4);
      r.Bits(0, 1);  // not a general line
      if (dx) r.Bits(0, 1), r.Signed(dx, n);
      else r.Bits(1, 1), r.Signed(dy, n);
    };
    line(bounds.x1 - bounds.x0, 0);
    line(0, bounds.y1 - bounds.y0);
    line(bounds.x0 - bounds.x1, 0);
    line(0, bounds.y0 - bounds.y1);
    r.Bits(0, 1), r.Bits(0, 5);  // end
    s.insert(s.end(), r.out.begin(), r.out.end());
  }
  PutTag(o, 32, s);
}

bool IsShapeTag(uint16_t code) { return code == 2 || code == 22 || code == 32 || code == 83; }

}  // namespace

uint64_t ShapeHash(uint16_t code, std::span<const uint8_t> tag_after_id) {
  uint64_t h = 0xCBF29CE484222325ull;
  auto mix = [&](uint8_t b) { h ^= b, h *= 0x100000001B3ull; };
  mix(uint8_t(code));
  for (uint8_t b : tag_after_id) mix(b);
  return h;
}

bool ParseRules(std::span<const uint8_t> blob, std::vector<Rule>& rules) {
  if (blob.size() < 12 || std::memcmp(blob.data(), "CD2P", 4) != 0 || Le32(&blob[4]) != 1) return false;
  const uint32_t n = Le32(&blob[8]);
  size_t p = 12;
  rules.clear();
  for (uint32_t i = 0; i < n; ++i) {
    if (p + 16 > blob.size()) return false;
    Rule r;
    r.hash = uint64_t(Le32(&blob[p])) | uint64_t(Le32(&blob[p + 4])) << 32;
    r.width = Le16(&blob[p + 8]), r.height = Le16(&blob[p + 10]);
    const uint32_t len = Le32(&blob[p + 12]);
    p += 16;
    if (p + len > blob.size() || !r.width || !r.height) return false;
    r.zlib_argb.assign(blob.begin() + p, blob.begin() + p + len);
    p += len;
    rules.push_back(std::move(r));
  }
  return true;
}

std::vector<uint8_t> PatchMovie(std::span<const uint8_t> movie, const std::vector<Rule>& rules, int& replaced) {
  replaced = 0;
  if (movie.size() < 8) return {};
  const bool compressed = movie[0] == 'C';
  if (!((movie[0] == 'C' || movie[0] == 'G' || movie[0] == 'F') && (movie[1] == 'F' || movie[1] == 'W') &&
        (movie[2] == 'X' || movie[2] == 'S')))
    return {};
  std::vector<uint8_t> body;
  if (compressed) {
    int len = 0;
    char* raw = stbi_zlib_decode_malloc(reinterpret_cast<const char*>(movie.data() + 8), int(movie.size() - 8), &len);
    if (!raw) return {};
    body.assign(raw, raw + len);
    std::free(raw);
  } else {
    body.assign(movie.begin() + 8, movie.end());
  }
  Rect frame;
  if (!ReadRect(body.data(), body.size(), frame) || frame.bytes + 4 > body.size()) return {};
  std::vector<uint8_t> out(movie.begin(), movie.begin() + 8);
  out[0] = movie[0] == 'C' ? (movie[1] == 'F' ? 'G' : 'F') : movie[0];  // (CFX -> GFX, CWS -> FWS)
  out.insert(out.end(), body.begin(), body.begin() + frame.bytes + 4);
  size_t p = frame.bytes + 4;
  uint16_t next_bitmap = 0x7000;  // (the movies' own ids end at 234)
  while (p + 2 <= body.size()) {
    const size_t start = p;
    const uint16_t h = Le16(&body[p]);
    p += 2;
    const uint16_t code = h >> 6;
    uint32_t len = h & 0x3F;
    if (len == 0x3F) {
      if (p + 4 > body.size()) return {};
      len = Le32(&body[p]);
      p += 4;
    }
    if (p + len > body.size()) return {};
    bool done = false;
    if (IsShapeTag(code) && len > 2) {
      const uint64_t hash = ShapeHash(code, std::span<const uint8_t>(&body[p + 2], len - 2));
      for (const Rule& r : rules) {
        if (r.hash != hash) continue;
        Rect b;
        if (!ReadRect(&body[p + 2], len - 2, b)) break;
        PutReplacement(out, Le16(&body[p]), &body[p + 2], b, next_bitmap++, r);
        ++replaced;
        done = true;
        break;
      }
    }
    if (!done) out.insert(out.end(), body.begin() + start, body.begin() + p + len);
    p += len;
    if (code == 0) break;
  }
  if (!replaced) return {};
  SetLe32(&out[4], uint32_t(out.size()));
  return out;
}

bool PatchPack(std::span<const uint8_t> toc, std::span<const uint8_t> pack, const std::vector<Rule>& rules,
               PackResult& out, std::string& error) {
  if (toc.size() < 0x20 || std::memcmp(toc.data(), "PACK", 4) != 0) return error = "not a PACK table", false;
  const uint32_t n = Be32(&toc[12]);
  const uint32_t align = std::max<uint32_t>(Be32(&toc[16]), 1);
  const size_t names = 0x20 + size_t(n) * 16;
  if (names > toc.size()) return error = "table too short", false;
  out = {};
  out.toc.assign(toc.begin(), toc.end());
  std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, uint32_t>> placed;  // old (offset, size) -> new
  for (uint32_t i = 0; i < n; ++i) {
    uint8_t* e = &out.toc[0x20 + size_t(i) * 16];
    const uint32_t name_off = Be32(e), off = Be32(e + 4), size = Be32(e + 8);
    if (size_t(off) + size > pack.size()) return error = "entry past the pack's end", false;
    if (auto it = placed.find({off, size}); it != placed.end()) {
      SetBe32(e + 4, it->second.first), SetBe32(e + 8, it->second.second);
      continue;
    }
    // the name (UTF-16BE): ends in ".gfx"?
    std::string name;
    for (size_t q = names + size_t(name_off) * 2; q + 1 < toc.size() && (toc[q] || toc[q + 1]); q += 2)
      name.push_back(toc[q] ? '?' : char(toc[q + 1]));
    std::vector<uint8_t> patched;
    if (name.size() > 4) {
      std::string ext = name.substr(name.size() - 4);
      std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return char(std::tolower(uint8_t(c))); });
      if (ext == ".gfx") {
        int replaced = 0;
        patched = PatchMovie(pack.subspan(off, size), rules, replaced);
        if (replaced) ++out.movies, out.shapes += replaced;
      }
    }
    const std::span<const uint8_t> data = patched.empty() ? pack.subspan(off, size) : std::span<const uint8_t>(patched);
    while (out.pack.size() % align) out.pack.push_back(0xFA);
    const uint32_t new_off = uint32_t(out.pack.size());
    out.pack.insert(out.pack.end(), data.begin(), data.end());
    placed[{off, size}] = {new_off, uint32_t(data.size())};
    SetBe32(e + 4, new_off), SetBe32(e + 8, uint32_t(data.size()));
  }
  while (out.pack.size() % align) out.pack.push_back(0xFA);
  return true;
}

}  // namespace cd2::prompts
