// Crackdown 2 - the quick menu's cheats (see cd2_cheats.h).
#include "cd2_cheats.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include <rex/logging.h>
#include <rex/memory/utils.h>

namespace cd2::cheats {
namespace {

constexpr uint32_t kPlayerGlobal = 0x83800F88;  // the player (lis -31872, lwz 3976)
constexpr uint32_t kPlayerCharacterRef = 136;    // its character, 28 bytes into the cCharacter
constexpr uint32_t kCharacterBase = 28;
constexpr uint32_t kFlags = 10261;               // the character's debug flags byte
constexpr uint8_t kGodMode = 0x20, kInfiniteAmmo = 0x10;

struct Info {
  const char* label;
  const char* section;
  const char* key;
  uint8_t bit;
};
constexpr Info kInfo[] = {
    {"Infinite Health", "PLAYER", "cheat_infinite_health", kGodMode},
    {"Infinite Ammo", "WEAPONS", "cheat_infinite_ammo", kInfiniteAmmo},
};

std::atomic<bool> g_on[int(Cheat::kCount)] = {};
std::atomic<bool> g_available{false};
// per cheat: the character whose bit the cheat set (0: none), the game thread's
uint32_t g_set_on[int(Cheat::kCount)] = {};

bool Plausible(uint32_t a) { return a >= 0x40000000u && a < 0xA0000000u; }
uint32_t Load32(uint8_t* base, uint32_t a) { return rex::memory::load_and_swap<uint32_t>(base + a); }

uint32_t PlayerCharacter(uint8_t* base) {
  const uint32_t player = Load32(base, kPlayerGlobal);
  if (!Plausible(player)) return 0;
  const uint32_t ref = Load32(base, player + kPlayerCharacterRef);
  return Plausible(ref) ? ref - kCharacterBase : 0;
}

}  // namespace

const char* Label(Cheat c) { return kInfo[int(c)].label; }
const char* Section(Cheat c) { return kInfo[int(c)].section; }
bool On(Cheat c) { return g_on[int(c)].load(std::memory_order_relaxed); }
void Toggle(Cheat c) { g_on[int(c)].store(!On(c), std::memory_order_relaxed); }
bool Available() { return g_available.load(std::memory_order_relaxed); }

void Load(const std::filesystem::path& file) {
  std::FILE* f = std::fopen(file.string().c_str(), "rb");
  if (!f) return;
  char line[256];
  while (std::fgets(line, sizeof(line), f)) {
    char key[64] = {};
    int value = 0;
    if (std::sscanf(line, " %63[a-z_] = %d", key, &value) != 2) continue;
    for (int i = 0; i < int(Cheat::kCount); ++i)
      if (std::strcmp(key, kInfo[i].key) == 0) g_on[i].store(value != 0);
  }
  std::fclose(f);
}

void Save(const std::filesystem::path& file) {
  std::FILE* f = std::fopen(file.string().c_str(), "wb");
  if (!f) {
    REXLOG_WARN("Crackdown 2 cheats: cannot write {}", file.string());
    return;
  }
  for (int i = 0; i < int(Cheat::kCount); ++i) std::fprintf(f, "%s = %d\n", kInfo[i].key, On(Cheat(i)) ? 1 : 0);
  std::fclose(f);
}

void Tick(uint8_t* base) {
  const uint32_t character = PlayerCharacter(base);
  g_available.store(character != 0, std::memory_order_relaxed);
  for (int i = 0; i < int(Cheat::kCount); ++i) {
    const uint8_t bit = kInfo[i].bit;
    uint32_t& mine = g_set_on[i];
    if (mine && mine != character) mine = 0;  // (another character now: the old one's bit is the game's)
    if (!character) continue;
    uint8_t* flags = base + character + kFlags;
    if (On(Cheat(i))) {
      if (!(*flags & bit)) {
        *flags |= bit;
        if (!mine) REXLOG_INFO("Crackdown 2 cheats: {} on (character {:08X})", kInfo[i].label, character);
        mine = character;
      }
      // (already set and not by us: the game's own, left to it; if it clears it, the line above sets it)
    } else if (mine) {
      *flags &= uint8_t(~bit);
      REXLOG_INFO("Crackdown 2 cheats: {} off", kInfo[i].label);
      mine = 0;
    }
  }
}

}  // namespace cd2::cheats
