// Crackdown 2's PlayStation button prompts - the part that needs nothing of the runtime (cd2_prompts.cpp has the
// rest), so it builds and is tested on a PC too (tools/test_cd2_prompts.cpp).
//
// Crackdown 2 draws its buttons as vector shapes in its Scaleform movies (.gfx inside UI\ui.pack, pause.pack,
// initial.pack). tools/make_cd2_prompts.py lists them by a hash of their DefineShape tag and draws the PlayStation
// one for each: /app0/glyphs/cd2_prompts.bin. Here a pack is copied with each matching shape replaced by one of
// the same id and bounds filled with that picture (a DefineBitsLossless2 + DefineShape3), the movie written back
// uncompressed ("GFX"), and the pack's table of contents given the new offsets and sizes.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace cd2::prompts {

struct Rule {
  uint64_t hash = 0;
  uint16_t width = 0, height = 0;
  std::vector<uint8_t> zlib_argb;  // DefineBitsLossless2 format 5's data
};

// cd2_prompts.bin: false when it isn't one.
bool ParseRules(std::span<const uint8_t> blob, std::vector<Rule>& rules);

// FNV-1a 64 of the tag code byte and the tag's bytes after the shape id (make_cd2_prompts.py's key).
uint64_t ShapeHash(uint16_t code, std::span<const uint8_t> tag_after_id);

// One movie (.gfx/.swf, compressed or not) with its matching shapes replaced; empty when nothing matched or it
// isn't a movie. `replaced` counts the shapes.
std::vector<uint8_t> PatchMovie(std::span<const uint8_t> movie, const std::vector<Rule>& rules, int& replaced);

struct PackResult {
  std::vector<uint8_t> toc, pack;
  int movies = 0, shapes = 0;
};
// A pack and its .toc (Crackdown 2's "PACK" v2, big-endian, UTF-16 names) with every .gfx in it patched. False
// when the files aren't what's expected; `out.shapes == 0` when nothing in it matched.
bool PatchPack(std::span<const uint8_t> toc, std::span<const uint8_t> pack, const std::vector<Rule>& rules,
               PackResult& out, std::string& error);

}  // namespace cd2::prompts
