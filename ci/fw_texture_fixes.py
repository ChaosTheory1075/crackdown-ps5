#!/usr/bin/env python3
"""Textures the CPU rewrites, kept right (from Saints Row: ReInstated's ci/patch_framework.py, lines 179-483 and
1715-1777): Crackdown 2's agent heads showed a bar of static that came and went - the GPU's copy of a page the game
rewrote with no write watch fired (crackdown2_013: "GPU copy of physical 1A048000 +4000 out of date, no write watch
fired", each time the heads were on screen) was drawn until the stale page scan came round to it.
- Each frame's first draw that binds a texture compares its pages with their upload hashes and reloads it if they
  changed (CheckDynamicRange; linear and movie-plane textures by their data hash).
- Xenia Canary 9781a75: a texture invalidated while it loaded stays outdated instead of kept stale.
cd_texture_recheck (default true) turns both off; cd_texture_recheck_sticky (default true) keeps a page once caught
changing with no watch fired re-checked every frame, instead of for 300 frames (the heads' page 1A048000 went stale
again and again, each time caught only by the slow scan).

    fw_texture_fixes.py <framework>     (after patch_framework.py and fw_perf_fixes.py)
"""
import pathlib
import sys


def edit(path, old, new, count=1):
    text = path.read_text()
    if new in text:
        print(f"already  {path}")
        return
    if text.count(old) != count:
        sys.exit(f"fw_texture_fixes: expected {count} of {old!r} in {path}, found {text.count(old)}")
    path.write_text(text.replace(old, new))
    print(f"patched  {path}")


def main():
    fw = pathlib.Path(sys.argv[1])
    sdk = fw / "glue" / "rexglue-sdk-main"
    tc = sdk / "src" / "graphics" / "pipeline" / "texture" / "cache.cpp"
    edit(tc, "TextureCache::BeforeTextureReadCallback g_before_texture_read = nullptr;\n}  // namespace\n",
         "TextureCache::BeforeTextureReadCallback g_before_texture_read = nullptr;\n}  // namespace\n"
         "}  // namespace rex::graphics\n"
         "REXCVAR_DEFINE_BOOL(cd_texture_recheck, true, \"GPU\",\n"
         "                    \"Re-check CPU-written textures once a frame and reload them if their pages changed \"\n"
         "                    \"with no write watch fired (Crackdown 2's agent-head static); Xenia 9781a75\");\n"
         "namespace rex::graphics {\n")
    # Cutscene movies (CRI Sofdec, decoded by the CPU into the same few
    # textures every frame) froze for up to 9 s while their sound went on:
    # the frames reached the GPU only when a write watch happened to fire or
    # the stale page scan (128 pages a frame: all memory in about 8.5 s at
    # 30 FPS) came round to them. sr_004.log: the title movie's planes
    # reloaded every frame on its first showing, and the scan found the
    # cutscene movie's pages (08B74000-08B7C000) changed with no watch fired.
    # Whatever leaves a page unwatched (the 16 KB host pages hold four guest
    # pages, the E0 view is 4 KB off the others, the hot page list fills), a
    # texture the CPU rewrites no longer depends on the watches: the first
    # draw of each frame that binds it compares its pages with the hashes
    # taken at their upload, and a changed page is invalidated as a fired
    # watch would, so the texture is reloaded for that draw. Checked: linear
    # textures (the movie's planes; the game's other textures are tiled) and
    # textures on pages the CPU changed in the last 300 frames; at most 16 MB
    # hashed a frame.
    # [cd] A 16 KB host page was watched only if the FIRST 4 KB guest page in it was writable: one read-only or
    # inaccessible guest page at its start left the other three unwatched, and their writes went unseen until the
    # stale page scan (crackdown2_013/014: "out of date, no write watch fired" on many pages; the E0 view, 4 KB off the
    # others, shifts which guest page is first). Now any writable guest page in the host page gets it watched.
    # cd_watch_any_guest_page (default true).
    xm = sdk / "src" / "system" / "xmemory.cpp"
    edit(xm, 'REXCVAR_DEFINE_BOOL(scribble_heap, false, "Memory", "Scribble 0xCD into all allocated heap memory");\n',
         'REXCVAR_DEFINE_BOOL(scribble_heap, false, "Memory", "Scribble 0xCD into all allocated heap memory");\n'
         'REXCVAR_DEFINE_BOOL(cd_watch_any_guest_page, true, "Memory",  // [cd]\n'
         '                    "A host page is write-watched if any guest page in it is writable, not only its first");\n')
    edit(xm, "        ToPageAccess(page_table_[guest_page_number].current_protect);\n"
             "    bool protect_system_page = false;\n",
         "        ToPageAccess(page_table_[guest_page_number].current_protect);\n"
         "    if (REXCVAR_GET(cd_watch_any_guest_page) && current_page_access != rex::memory::PageAccess::kReadWrite) {\n"
         "      // [cd] any writable guest page in this host page (not only the first) gets it watched\n"
         "      const uint32_t cd_guest_end =\n"
         "          std::min<uint32_t>(uint32_t(page_table_.size()),\n"
         "                             (rex::sat_sub((i + 1) * system_page_size_, host_address_offset()) +\n"
         "                              (uint32_t(1) << page_size_shift_) - 1) >> page_size_shift_);\n"
         "      for (uint32_t g = guest_page_number + 1; g < cd_guest_end; ++g) {\n"
         "        const rex::memory::PageAccess a = ToPageAccess(page_table_[g].current_protect);\n"
         "        if (a == rex::memory::PageAccess::kReadWrite || a == rex::memory::PageAccess::kExecuteReadWrite) {\n"
         "          current_page_access = rex::memory::PageAccess::kReadWrite;\n"
         "          break;\n"
         "        }\n"
         "      }\n"
         "    }\n"
         "    bool protect_system_page = false;\n")
    sm_h = sdk / "include" / "rex" / "graphics" / "shared_memory.h"
    edit(sdk / "src" / "graphics" / "shared_memory.cpp", "namespace rex::graphics {\n",
         "#include <rex/cvar.h>  // [cd]\n"
         "REXCVAR_DEFINE_BOOL(cd_texture_recheck_sticky, true, \"GPU\",\n"
         "                    \"A page once found changed with no write watch fired is re-checked at every frame's \"\n"
         "                    \"first draw that binds it, not only for 300 frames (Crackdown 2's agent-head static)\");\n\n"
         "namespace rex::graphics {\n")
    edit(sm_h,
         """  uint32_t CheckStalePages(uint32_t pages_per_frame);
""",
         """  uint32_t CheckStalePages(uint32_t pages_per_frame);

  // [cd] The pages of a texture's range compared with their hashes taken at upload (once a
  // frame a texture, from TextureCache::RequestTextures); a changed page is invalidated as a
  // fired write watch would. always: checked even if the CPU didn't change its pages lately.
  // Returns whether any page was invalidated.
  bool CheckDynamicRange(uint32_t start, uint32_t length, bool always);
  uint32_t frame() const { return hot_frame_.load(std::memory_order_relaxed); }
  // [cd] A linear texture (a movie frame's plane): its guest data hashed (the movie log), and
  // its range invalidated so that it's reloaded from guest memory, whatever the watches say.
  uint64_t HashGuestRange(uint32_t start, uint32_t length);
  void InvalidateRangeNow(uint32_t start, uint32_t length);
""")
    edit(sm_h,
         """  std::vector<uint32_t> hot_page_checked_batch_;
""",
         """  std::vector<uint32_t> hot_page_checked_batch_;
  // [cd] CheckDynamicRange's budget
  static constexpr uint32_t kDynamicCheckBytesPerFrame = 16u << 20;
  uint32_t dynamic_check_frame_ = 0;
  uint32_t dynamic_check_bytes_ = 0;
""")
    edit(sdk / "src" / "graphics" / "shared_memory.cpp",
         """namespace {
thread_local bool invalidation_from_write = false;  // [crackdown]
""",
         """bool SharedMemory::CheckDynamicRange(uint32_t start, uint32_t length, bool always) {
  // [cd]
  if (!length || start >= kBufferSize || page_hashes_.size() != (kBufferSize >> page_size_log2_)) {
    return false;
  }
  const uint32_t page_size = uint32_t(1) << page_size_log2_;
  const uint32_t frame = hot_frame_.load(std::memory_order_relaxed);
  const uint32_t page_first = start >> page_size_log2_;
  const uint32_t page_last = std::min(uint32_t((uint64_t(start) + length - 1) >> page_size_log2_),
                                      uint32_t(page_hashes_.size() - 1));
  if (!always) {
    bool recent = false;
    for (uint32_t page = page_first; page <= page_last && !recent; ++page) {
      const uint32_t last = page_invalidation_frame_[page];
      // [cd] sticky: a page once caught changing with no watch fired is checked at every frame's first draw
      recent = last && (REXCVAR_GET(cd_texture_recheck_sticky) || frame - last <= kHotPageQuietFrames);
    }
    if (!recent) {
      return false;
    }
  }
  if (dynamic_check_frame_ != frame) {
    dynamic_check_frame_ = frame;
    dynamic_check_bytes_ = 0;
  }
  bool invalidated = false;
  for (uint32_t page = page_first; page <= page_last; ++page) {
    if (dynamic_check_bytes_ >= kDynamicCheckBytesPerFrame) {
      break;
    }
    const uint64_t stored = page_hashes_[page];
    if (!stored) {
      continue;
    }
    const uint32_t address = page << page_size_log2_;
    const uint32_t block = page >> 6;
    const uint64_t bit = uint64_t(1) << (page & 63);
    {
      // (as CheckStalePages: a valid page is committed; GPU-written ones and those the guest made
      // unreadable are left alone)
      auto global_lock = global_critical_region_.Acquire();
      const uint64_t* valid_flags = active_valid_flags_.load(std::memory_order_relaxed);
      if (!valid_flags || !(valid_flags[block] & bit) ||
          (system_page_flags_valid_and_gpu_written_[block] & bit)) {
        continue;
      }
      uint32_t protect = 0;
      auto* physical_heap = memory_.GetPhysicalHeap();
      if (!physical_heap || !physical_heap->QueryProtect(address, &protect) ||
          !(protect & memory::kMemoryProtectRead)) {
        continue;
      }
      dynamic_check_bytes_ += page_size;
      if (XXH3_64bits(memory_.TranslatePhysical(address), page_size) == stored) {
        continue;
      }
    }
    page_hashes_[page] = 0;
    memory_.TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                           0xA0000000u + address, page_size, true, true);
    memory_.TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                           0xC0000000u + address, page_size, true, true);
    if (address >= 0x1000) {
      memory_.TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                             0xE0000000u + address - 0x1000, page_size, true, true);
    }
    MemoryInvalidationCallback(address, page_size, true);
    invalidated = true;
  }
  if (invalidated) {
    static std::atomic<uint64_t> dynamic_reloads{0};
    const uint64_t n = ++dynamic_reloads;
    if (n <= 5 || !(n & (n - 1))) {
      REXGPU_INFO("sr: a CPU-written texture at {:08X} changed with no write watch fired ({} so far)",
                  start, n);
    }
  }
  return invalidated;
}

uint64_t SharedMemory::HashGuestRange(uint32_t start, uint32_t length) {
  // [cd]
  if (!length || start >= kBufferSize) {
    return 0;
  }
  length = std::min(length, kBufferSize - start);
  return XXH3_64bits(memory_.TranslatePhysical(start), length);
}

void SharedMemory::InvalidateRangeNow(uint32_t start, uint32_t length) {
  // [cd] (as CheckStalePages does for a changed page, each physical view's watches included)
  if (!length || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  const uint32_t page_size = uint32_t(1) << page_size_log2_;
  const uint32_t first = start & ~(page_size - 1);
  const uint32_t size = ((start + length - first) + page_size - 1) & ~(page_size - 1);
  for (uint32_t page = first >> page_size_log2_; page < ((first + size) >> page_size_log2_) &&
                                                 page < page_hashes_.size();
       ++page) {
    page_hashes_[page] = 0;
  }
  memory_.TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                         0xA0000000u + first, size, true, true);
  memory_.TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                         0xC0000000u + first, size, true, true);
  if (first >= 0x1000) {
    memory_.TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                           0xE0000000u + first - 0x1000, size, true, true);
  }
  MemoryInvalidationCallback(first, size, true);
}

namespace {
thread_local bool invalidation_from_write = false;  // [crackdown]
""")
    tc_h = sdk / "include" / "rex" / "graphics" / "pipeline" / "texture" / "cache.h"
    edit(tc_h,
         """    void MakeUpToDateAndWatch(const rex::thread::global_lock_t& global_lock);
""",
         """    void MakeUpToDateAndWatch(const rex::thread::global_lock_t& global_lock);
    // [cd] the frame its pages were last compared with their upload hashes; a linear one's data
    // hash then (the movie log)
    uint32_t dynamic_check_frame = 0;
    uint64_t dynamic_content_hash = 0;
""")
    edit(tc_h,
         """  uint64_t current_submission_index_ = 0;
""",
         """  uint64_t current_submission_index_ = 0;
  uint32_t dynamic_check_frame_ = 0;  // [cd]
""")
    tc = sdk / "src" / "graphics" / "pipeline" / "texture" / "cache.cpp"
    # The movie log: each second a linear texture is drawn, how many frames drew it and in how
    # many of them the game had written new data into it. A freeze with new data each frame is
    # the GPU's side; one with none, the game's (its decoder or its clock).
    edit(tc,
         """void TextureCache::RequestTextures(uint32_t used_texture_mask) {
""",
         """namespace {
// [cd] the movie log
struct MovieLogEntry {
  uint32_t base = 0, frames = 0, changed = 0, last_frame = 0;
};
MovieLogEntry g_movie_log[24];
std::chrono::steady_clock::time_point g_movie_log_time;
void MovieLogFrame(uint32_t base, uint32_t frame, bool changed) {
  const auto now = std::chrono::steady_clock::now();
  if (now - g_movie_log_time >= std::chrono::seconds(1)) {
    std::string line;
    for (MovieLogEntry& e : g_movie_log) {
      if (e.frames) {
        line += fmt::format(" {:08X}: {} new of {}", e.base, e.changed, e.frames);
      }
      e = MovieLogEntry();
    }
    if (!line.empty()) {
      REXGPU_INFO("sr movie 1 s (linear, tiled k_8 (+1), 256 KB+ (+2, new: outdated by a write watch) textures: frames with new data of frames drawn):{}", line);
    }
    g_movie_log_time = now;
  }
  MovieLogEntry* slot = nullptr;
  for (MovieLogEntry& e : g_movie_log) {
    if (e.frames && e.base == base) {
      slot = &e;
      break;
    }
    if (!slot && !e.frames) {
      slot = &e;
    }
  }
  if (!slot || slot->last_frame == frame + 1) {
    return;
  }
  slot->base = base;
  slot->last_frame = frame + 1;
  ++slot->frames;
  slot->changed += changed ? 1 : 0;
}
}  // namespace

void TextureCache::RequestTextures(uint32_t used_texture_mask) {
""")
    edit(tc, "#include <mutex>\n", "#include <chrono>\n#include <mutex>\n#include <string>\n\n#include <fmt/format.h>\n")
    edit(tc,
         """    ResetTextureBindings();
  }

  // Update the texture keys and the textures.
""",
         """    ResetTextureBindings();
  }

  // [cd] each frame's first draws look at every binding again (CheckDynamicRange)
  const uint32_t dynamic_frame = shared_memory().frame();
  if (dynamic_frame != dynamic_check_frame_) {
    dynamic_check_frame_ = dynamic_frame;
    texture_bindings_in_sync_ = 0;
  }

  // Update the texture keys and the textures.
""")
    edit(tc,
         """    if (g_before_texture_read) {
      Texture* loading = load_unsigned_data ? binding.texture
""",
         """    // [cd] a texture the CPU rewrites (a cutscene movie's frame): its pages compared with their
    // upload hashes once a frame, reloaded if they changed with no write watch fired
    for (Texture* dynamic : {binding.texture, binding.texture_signed}) {
      if (!REXCVAR_GET(cd_texture_recheck)) break;  // [cd] the switch
      // [cd] every texture of 256 KB or more in the movie log too, whether a write watch
      // outdated it this frame (+2): a cutscene's planes, whatever their format
      if (dynamic && dynamic->dynamic_check_frame != dynamic_frame && dynamic->GetGuestBaseSize() >= (256u << 10)) {
        const TextureKey big_key = dynamic->key();
        if (big_key.tiled && big_key.format == xenos::TextureFormat::k_8) {
        } else if (big_key.tiled || (dynamic->outdated_mask() & Texture::kOutdatedBitBase)) {
          MovieLogFrame((big_key.base_page << 12) | 2u | (big_key.tiled ? 1u : 0u), dynamic_frame,
                        (dynamic->outdated_mask() & Texture::kOutdatedBitBase) != 0);
        }
      }
      if (!dynamic || dynamic->dynamic_check_frame == dynamic_frame ||
          (dynamic->outdated_mask() & Texture::kOutdatedBitBase)) {
        continue;
      }
      dynamic->dynamic_check_frame = dynamic_frame;
      const TextureKey dynamic_key = dynamic->key();
      if (dynamic_key.scaled_resolve || !dynamic->GetGuestBaseSize()) {
        continue;
      }
      // [cd] Bink's planes on the 360 are tiled k_8 textures the CPU decodes into (Saints Row 2's
      // cutscenes stayed black: the first, black, frame was all the GPU ever saw): those too are
      // compared with their last data once a frame, and reloaded when it changed
      const bool movie_plane = !dynamic_key.tiled || (dynamic_key.format == xenos::TextureFormat::k_8 &&
                                                      dynamic->GetGuestBaseSize() <= (8u << 20));
      if (movie_plane) {
        // [cd] a linear texture (the movie's planes): reloaded every frame, and whether its data
        // changed counted for the movie log (a tiled one: only when it changed; logged with +1)
        const uint32_t base = dynamic_key.base_page << 12;
        const uint64_t hash = shared_memory().HashGuestRange(base, dynamic->GetGuestBaseSize());
        // (only when the guest's data changed, a linear one too, and not on the first look: a frame
        // the GPU resolved into it (Saints Row 2's Bink cutscenes) isn't in guest memory, and
        // reloading from there every frame put back the black one under it)
        const bool first = dynamic->dynamic_content_hash == 0;
        const bool changed = hash != dynamic->dynamic_content_hash;
        MovieLogFrame(base | (dynamic_key.tiled ? 1u : 0u), dynamic_frame, changed && !first);
        dynamic->dynamic_content_hash = hash;
        if (changed && !first) {
          shared_memory().InvalidateRangeNow(base, dynamic->GetGuestBaseSize());
          (dynamic == binding.texture ? load_unsigned_data : load_signed_data) = true;
        }
        continue;
      }
      if (shared_memory().CheckDynamicRange(dynamic_key.base_page << 12, dynamic->GetGuestBaseSize(),
                                            !dynamic_key.tiled)) {
        (dynamic == binding.texture ? load_unsigned_data : load_signed_data) = true;
      }
    }
    if (g_before_texture_read) {
      Texture* loading = load_unsigned_data ? binding.texture
""")
    # Xenia Canary 9781a75 "Avoid stale texture state after invalidation" (2026-06-29): a texture
    # is marked up to date and watched only if its shared memory range is still valid; one
    # invalidated meanwhile (the CPU rewrote it as it loaded: a streamed mip, a movie frame) stays
    # outdated and is loaded again, instead of being kept stale with a watch that won't fire.
    smh = sdk / "include" / "rex" / "graphics" / "shared_memory.h"
    edit(smh, "  bool RequestRange(uint32_t start, uint32_t length);\n",
         "  bool RequestRange(uint32_t start, uint32_t length);\n"
         "  // [cd] Whether every page of the range is valid in the GPU's copy (Xenia 9781a75).\n"
         "  bool IsRangeValid(uint32_t start, uint32_t length) const;\n")
    smc = sdk / "src" / "graphics" / "shared_memory.cpp"
    edit(smc, "bool SharedMemory::RequestRange(uint32_t start, uint32_t length) {\n",
         "bool SharedMemory::IsRangeValid(uint32_t start, uint32_t length) const {  // [cd]\n"
         "  if (!length) return true;\n"
         "  if (start > kBufferSize || (kBufferSize - start) < length) return false;\n"
         "  const uint64_t* valid = active_valid_flags_.load(std::memory_order_acquire);\n"
         "  if (!valid) return false;\n"
         "  const uint32_t page_first = start >> page_size_log2_;\n"
         "  const uint32_t page_last = (start + length - 1) >> page_size_log2_;\n"
         "  for (uint32_t i = page_first >> 6; i <= (page_last >> 6); ++i) {\n"
         "    uint64_t mask = UINT64_MAX;\n"
         "    if (i == (page_first >> 6)) mask &= ~((uint64_t(1) << (page_first & 63)) - 1);\n"
         "    if (i == (page_last >> 6) && (page_last & 63) != 63) mask &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;\n"
         "    if ((valid[i] & mask) != mask) return false;\n"
         "  }\n"
         "  return true;\n"
         "}\n\n"
         "bool SharedMemory::RequestRange(uint32_t start, uint32_t length) {\n")
    edit(sdk / "include" / "rex" / "graphics" / "pipeline" / "texture" / "cache.h",
         "    void MakeUpToDateAndWatch(const rex::thread::global_lock_t& global_lock);\n",
         "    bool MakeUpToDateAndWatch(const rex::thread::global_lock_t& global_lock);  // [cd]\n")
    tcc0 = sdk / "src" / "graphics" / "pipeline" / "texture" / "cache.cpp"
    edit(tcc0,
         "void TextureCache::Texture::MakeUpToDateAndWatch(\n"
         "    const rex::thread::global_lock_t& global_lock) {\n"
         "  SharedMemory& shared_memory = texture_cache().shared_memory();\n"
         "  if (base_outdated_) {\n",
         "bool TextureCache::Texture::MakeUpToDateAndWatch(  // [cd] (Xenia 9781a75)\n"
         "    const rex::thread::global_lock_t& global_lock) {\n"
         "  SharedMemory& shared_memory = texture_cache().shared_memory();\n"
         "  if (base_outdated_ && REXCVAR_GET(cd_texture_recheck) &&\n"
         "      !shared_memory.IsRangeValid(key().base_page << 12, rex::align(GetGuestBaseSize(), UINT32_C(16)))) {\n"
         "    return false;\n"
         "  }\n"
         "  if (mips_outdated_ && REXCVAR_GET(cd_texture_recheck) &&\n"
         "      !shared_memory.IsRangeValid(key().mip_page << 12, rex::align(GetGuestMipsSize(), UINT32_C(16)))) {\n"
         "    return false;\n"
         "  }\n"
         "  if (base_outdated_) {\n")
    edit(tcc0,
         "    outdated_mask_.fetch_and(~kOutdatedBitMips, std::memory_order_release);\n"
         "  }\n"
         "}\n",
         "    outdated_mask_.fetch_and(~kOutdatedBitMips, std::memory_order_release);\n"
         "  }\n"
         "  return true;\n"
         "}\n")
    edit(tcc0,
         "  texture.MakeUpToDateAndWatch(global_critical_region_.Acquire());\n"
         "  texture.LogAction(\"Loaded\");\n",
         "  if (!texture.MakeUpToDateAndWatch(global_critical_region_.Acquire())) {  // [cd]\n"
         "    return false;\n"
         "  }\n"
         "  texture.LogAction(\"Loaded\");\n")
    lane_hash_race(sdk, sm_h)


def lane_hash_race(sdk, sm_h):
    # [cd] "out of date, no write watch fired" with the GPU's second lane on (crackdown2_013/015, lane on in every
    # CD2 log): a page a fired watch invalidated keeps the hash of its last upload (MemoryInvalidationCallback leaves
    # page_hashes_ alone). At its reupload UploadRanges marks it valid at once, on the command thread, but its new
    # hash is taken later, on the lane (RecordPageHashesOnLane). In between, the stale page scan (at the swap), the
    # texture re-check and the hot page check, all on the command thread, compare the page's new data with the OLD
    # hash: a write the watch did see is reported as missed, and the page is invalidated and uploaded again.
    # cd_lane_hash_fix (default false, toml: cd_lane_hash_fix = true) clears the page's hash before the lane's job,
    # so the checks skip it until the lane has stored the new one (they all skip a page with no hash).
    # Each stale line now has a second, "cd stale page:", saying how long ago the page was uploaded (0-1 frames =
    # this race), whether it's a hot page (unwatched by design), the lane, and the watch flag in each of the three
    # physical views (all set = a writer that went past the protection; one missing = the watch bookkeeping).
    smc = sdk / "src" / "graphics" / "shared_memory.cpp"
    edit(smc, "namespace rex::graphics {\n",
         "REXCVAR_DEFINE_BOOL(cd_lane_hash_fix, false, \"GPU\",  // [cd]\n"
         "                    \"A page reuploaded through the GPU lane has no hash until the lane stores its new one \"\n"
         "                    \"(the stale checks compared new data with the old hash: false 'no write watch fired')\");\n\n"
         "namespace rex::graphics {\n")
    edit(sm_h, "  uint64_t HashGuestRange(uint32_t start, uint32_t length);\n",
         "  uint64_t HashGuestRange(uint32_t start, uint32_t length);\n"
         "  // [cd] (cd_lane_hash_fix) the pages' hashes cleared before the lane takes the new ones\n"
         "  void CdClearPageHashes(uint32_t page_first, uint32_t page_count);\n")
    edit(sm_h, "  uint32_t dynamic_check_bytes_ = 0;\n",
         "  uint32_t dynamic_check_bytes_ = 0;\n"
         "  std::vector<uint32_t> cd_page_upload_frame_;  // [cd] the frame each page's hash was last taken (stale log)\n")
    edit(smc, "  page_invalidation_frame_.assign(kBufferSize >> page_size_log2_, 0);\n",
         "  page_invalidation_frame_.assign(kBufferSize >> page_size_log2_, 0);\n"
         "  cd_page_upload_frame_.assign(kBufferSize >> page_size_log2_, 0);  // [cd]\n")
    edit(smc, "    page_hashes_[page] = hash ? hash : 1;\n",
         "    page_hashes_[page] = hash ? hash : 1;\n"
         "    if (page < cd_page_upload_frame_.size()) {\n"
         "      cd_page_upload_frame_[page] = hot_frame_.load(std::memory_order_relaxed);  // [cd]\n"
         "    }\n")
    edit(smc, "void SharedMemory::RecordPageHashesOnLane(uint32_t page_first, uint32_t page_count) {\n",
         "void SharedMemory::CdClearPageHashes(uint32_t page_first, uint32_t page_count) {  // [cd]\n"
         "  if (!REXCVAR_GET(cd_lane_hash_fix)) {\n"
         "    return;\n"
         "  }\n"
         "  for (uint32_t page = page_first; page_count && page < page_hashes_.size(); ++page, --page_count) {\n"
         "    page_hashes_[page] = 0;\n"
         "  }\n"
         "}\n\n"
         "void SharedMemory::RecordPageHashesOnLane(uint32_t page_first, uint32_t page_count) {\n")
    edit(sdk / "src" / "graphics" / "vulkan" / "shared_memory.cpp",
         "      if (lane) {\n"
         "        RecordPageHashesOnLane(upload_range_start, uint32_t(upload_buffer_size >> page_size_log2()));\n",
         "      if (lane) {\n"
         "        CdClearPageHashes(upload_range_start, uint32_t(upload_buffer_size >> page_size_log2()));  // [cd]\n"
         "        RecordPageHashesOnLane(upload_range_start, uint32_t(upload_buffer_size >> page_size_log2()));\n")
    edit(smc,
         "        REXGPU_WARN(\"GPU copy of physical {:08X} +{:X} out of date, no write watch fired ({} so far)\", address,\n"
         "                    page_size, stale_pages_found_);\n",
         "        REXGPU_WARN(\"GPU copy of physical {:08X} +{:X} out of date, no write watch fired ({} so far)\", address,\n"
         "                    page_size, stale_pages_found_);\n"
         "        // [cd] why: uploaded 0-1 frames ago = the lane's hash race; hot = unwatched by design; watched in all\n"
         "        // views (A0 C0 E0 = 7) = a writer past the protection; fewer = the watches' bookkeeping\n"
         "        const uint32_t cd_frame = hot_frame_.load(std::memory_order_relaxed);\n"
         "        const uint32_t cd_up = page < cd_page_upload_frame_.size() ? cd_page_upload_frame_[page] : 0;\n"
         "        const uint32_t cd_changed = page_invalidation_frame_[page];\n"
         "        REXGPU_WARN(\"cd stale page {:08X}: uploaded {} frames ago, last changed {}, hot {}, lane {}, \"\n"
         "                    \"watched views {} (A0 1, C0 2, E0 4)\",\n"
         "                    address, cd_up ? int64_t(cd_frame - cd_up) : int64_t(-1),\n"
         "                    cd_changed ? int64_t(cd_frame - cd_changed) : int64_t(-1),\n"
         "                    (hot_page_flags_[page >> 6] & bit) ? 1 : 0, GpuLane::Get().Active() ? 1 : 0,\n"
         "                    memory_.CdWatchState(address, page_size));\n")
    xh = sdk / "include" / "rex" / "system" / "xmemory.h"
    xc = sdk / "src" / "system" / "xmemory.cpp"
    edit(xh, "  bool TriggerCallbacks(rex::thread::global_lock_t global_lock_locked_once,\n",
         "  // [cd] whether every host page under the physical range has its write watch flag (the stale log)\n"
         "  bool CdAllWatched(uint32_t physical_address, uint32_t length);\n"
         "  bool TriggerCallbacks(rex::thread::global_lock_t global_lock_locked_once,\n")
    edit(xh, "  bool TriggerPhysicalMemoryCallbacks(\n",
         "  // [cd] the physical range's watch flags in each view: A0 1, C0 2, E0 4 (the stale log)\n"
         "  uint32_t CdWatchState(uint32_t physical_address, uint32_t length);\n"
         "  bool TriggerPhysicalMemoryCallbacks(\n")
    edit(xc, "bool PhysicalHeap::TriggerCallbacks(rex::thread::global_lock_t global_lock_locked_once,\n",
         "bool PhysicalHeap::CdAllWatched(uint32_t physical_address, uint32_t length) {  // [cd]\n"
         "  const uint32_t offset = GetPhysicalAddress(heap_base_);\n"
         "  if (!length || physical_address < offset || physical_address - offset >= heap_size_) {\n"
         "    return false;\n"
         "  }\n"
         "  const uint32_t rel = physical_address - offset;\n"
         "  length = std::min(length, heap_size_ - rel);\n"
         "  const uint32_t first = (rel + host_address_offset()) / system_page_size_;\n"
         "  const uint32_t last =\n"
         "      std::min((rel + length - 1 + host_address_offset()) / system_page_size_, system_page_count_ - 1);\n"
         "  auto global_lock = global_critical_region_.Acquire();\n"
         "  for (uint32_t i = first; i <= last; ++i) {\n"
         "    if (!(system_page_flags_[i >> 6].notify_on_invalidation & (uint64_t(1) << (i & 63)))) {\n"
         "      return false;\n"
         "    }\n"
         "  }\n"
         "  return true;\n"
         "}\n\n"
         "bool PhysicalHeap::TriggerCallbacks(rex::thread::global_lock_t global_lock_locked_once,\n")
    edit(xc, "bool Memory::TriggerPhysicalMemoryCallbacks(\n",
         "uint32_t Memory::CdWatchState(uint32_t physical_address, uint32_t length) {  // [cd]\n"
         "  return (heaps_.vA0000000.CdAllWatched(physical_address, length) ? 1u : 0u) |\n"
         "         (heaps_.vC0000000.CdAllWatched(physical_address, length) ? 2u : 0u) |\n"
         "         (heaps_.vE0000000.CdAllWatched(physical_address, length) ? 4u : 0u);\n"
         "}\n\n"
         "bool Memory::TriggerPhysicalMemoryCallbacks(\n")

if __name__ == "__main__":
    main()
