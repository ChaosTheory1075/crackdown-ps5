// Crackdown 2 (Title Update 5) - the quick menu's cheats (ps5/cd_quick_menu.cpp, built with CD_GAME=2).
//
// They switch the game's own debug flags on the player's character, the ones its script calls set (the
// cCharacter table sub_822DDB38 registers: "SetGodMode" 0x82112F50 ->
// sub_824610E0, "IsInGodMode" -> sub_825430B0, "SetInfiniteAmmo" 0x82112F68 -> sub_823E7638, "IsInfiniteAmmo" ->
// sub_825430C0). All four work on one byte of the character, +10261: god mode is bit 0x20, infinite ammo 0x10;
// the game's damage and weapon code reads them there (sub_8222F0E0, sub_826F47D8, sub_82748CC0, sub_8274AB40 ...).
// The player's character is [[0x83800F88] + 136] - 28, as the game itself gets it (308 places, e.g. 0x821E98xx).
//
// A cheat turned on sets its bit every frame (the game may clear it, a mission's script among them); turned off,
// it clears the bit only if the cheat set it, so a god mode the game gives itself (a cutscene) is left alone.
#pragma once

#include <cstdint>
#include <filesystem>

namespace cd2::cheats {

enum class Cheat : int { kInfiniteHealth, kInfiniteAmmo, kCount };

const char* Label(Cheat cheat);   // "Infinite Health" ...
const char* Section(Cheat cheat); // "PLAYER" / "WEAPONS"
bool On(Cheat cheat);
void Toggle(Cheat cheat);

// cheats2.ini: key = 0/1 lines.
void Load(const std::filesystem::path& file);
void Save(const std::filesystem::path& file);

// Whether a player character is there now (the menu's "the cheats act once you're playing" note).
bool Available();
// Once a frame, on the game's thread (cd2_features.cpp's end-of-frame hook).
void Tick(uint8_t* base);

}  // namespace cd2::cheats
