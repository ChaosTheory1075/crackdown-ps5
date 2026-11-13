// Crackdown PS5 - the in-game quick menu (see cd_quick_menu.h).
//
// The controller is read where the game reads it: sub_827CCB18, the title's XInputGetState (r3 the
// user, r4 the X_INPUT_STATE; the PC recomp's FilterPadState binding). After it runs, the menu takes
// what was pressed and, while open, blanks the state the game gets.
//
// Built into both programs. Crackdown 2's (CD_GAME=2) reads the pad at sub_829551D8, its XInputGetState
// (`mr r5,r4; li r4,1; b XamInputGetState`: r3 the user, r4 the state), which the game's pad poll calls for
// each of its four pads (sub_8232F710, LR 0x8232F768); its cheats are cd2_cheats.h's (the game's own god
// mode and infinite ammo flags), kept in /app0/cheats2.ini.
#include "cd_quick_menu.h"
#if REX_PLATFORM_PS5

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/sony_feedback.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#if defined(CD_GAME) && CD_GAME == 2
#include "cd2_cheats.h"
#define CD_QUICK_MENU_CD2 1
#else
#include "cheat_hooks.h"
#include "cheats.h"
#include "cheats_ps5.h"
#define CD_QUICK_MENU_CD2 0
#endif
#include "crackdown_menu.h"
#include "cd_text.h"

REXCVAR_DECLARE(std::string, crackdown_fps_counter);
REXCVAR_DEFINE_BOOL(crackdown_quick_menu, true, "Crackdown",
                    "L3 + R3 opens the in-game quick menu (the cheats); off: no menu, and no cheats");

namespace cdps5 {
namespace {

#if CD_QUICK_MENU_CD2
namespace cheats2 = cd2::cheats;
constexpr const char* kCheatsFile = "/app0/cheats2.ini";
#else
namespace cheats = crackdown::cheats;
constexpr const char* kCheatsFile = "/app0/cheats.ini";
#endif
constexpr const char* kMenuFile = "/app0/menu.ini";

// X_INPUT_GAMEPAD in the X_INPUT_STATE (big-endian): buttons, the triggers, the sticks.
constexpr uint32_t kGamepad = 4, kGamepadSize = 12, kThumbLY = 6;
constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kL3 = 0x0040, kR3 = 0x0080,
                   kCross = 0x1000, kCircle = 0x2000;
constexpr int kStickDeadzone = 16000;

enum Nav : uint8_t { kNavUp = 1, kNavDown, kNavLeft, kNavRight, kNavSelect, kNavClose };

std::atomic<bool> g_open{false};
std::mutex g_mutex;
std::deque<uint8_t> g_events;  // guarded by g_mutex
uint16_t g_previous = 0;       // the game thread's
bool g_swallow = false;        // blank the pad until everything is let go (after closing)

uint16_t LoadBe16(uint8_t* base, uint32_t address) {
  uint16_t v;
  std::memcpy(&v, base + address, 2);
  return std::byteswap(v);
}

void Push(uint8_t event) {
  std::lock_guard lock(g_mutex);
  if (g_events.size() < 16) g_events.push_back(event);
}

uint8_t Take() {
  std::lock_guard lock(g_mutex);
  if (g_events.empty()) return 0;
  const uint8_t e = g_events.front();
  g_events.pop_front();
  return e;
}

// One read of the controller, on the game thread.
void Filter(uint8_t* base, uint32_t state) {
  uint16_t buttons = LoadBe16(base, state + kGamepad);
  const int ly = int16_t(LoadBe16(base, state + kGamepad + kThumbLY));
  if (ly > kStickDeadzone) buttons |= kUp;  // (the left stick as the D-pad)
  if (ly < -kStickDeadzone) buttons |= kDown;
  const uint16_t pressed = buttons & ~g_previous;
  const bool combo = (buttons & (kL3 | kR3)) == (kL3 | kR3);
  const bool combo_pressed = combo && (pressed & (kL3 | kR3));
  g_previous = buttons;
  if (!g_open.load(std::memory_order_relaxed)) {
    if (combo_pressed && REXCVAR_GET(crackdown_quick_menu)) {
      { std::lock_guard lock(g_mutex); g_events.clear(); }
      g_open.store(true, std::memory_order_release);
      REXLOG_INFO("Quick menu: open");
    } else if (g_swallow && !buttons) {
      g_swallow = false;
    }
    if (!g_open.load(std::memory_order_relaxed) && !g_swallow) return;
  } else {
    if (combo_pressed || (pressed & kCircle)) {
      g_open.store(false, std::memory_order_release);
      g_swallow = true;
      Push(kNavClose);
    } else {
      if (pressed & kUp) Push(kNavUp);
      if (pressed & kDown) Push(kNavDown);
      if (pressed & kLeft) Push(kNavLeft);
      if (pressed & kRight) Push(kNavRight);
      if (pressed & kCross) Push(kNavSelect);
    }
  }
  std::memset(base + state + kGamepad, 0, kGamepadSize);
}

// menu.ini's `key` set to `value`, the other lines kept.
void SetMenuValue(const char* key, int value, const char* file = kMenuFile) {
  std::vector<std::string> lines;
  if (std::FILE* f = std::fopen(file, "rb")) {
    char line[256];
    while (std::fgets(line, sizeof(line), f)) lines.emplace_back(line);
    std::fclose(f);
  }
  const std::string prefix = std::string(key) + " =";
  const std::string entry = prefix + " " + std::to_string(value) + "\n";
  bool found = false;
  for (std::string& l : lines)
    if (l.rfind(prefix, 0) == 0) l = entry, found = true;
  if (!found) lines.push_back(entry);
  if (std::FILE* f = std::fopen(file, "wb")) {
    for (const std::string& l : lines) std::fputs(l.c_str(), f);
    std::fclose(f);
  }
}

// menu.ini's and the game's own file's `key` (Crackdown 2: menu2.ini, the set cd2.bin applies).
void SetMenuValues(const char* key, int value) {
  SetMenuValue(key, value);
#if CD_QUICK_MENU_CD2
  if (std::FILE* f = std::fopen("/app0/menu2.ini", "rb")) std::fclose(f), SetMenuValue(key, value, "/app0/menu2.ini");
#endif
}

// ---- the rows ----

// The PC recomp's cheat words (cheats.h, cheats.cpp: copied in by the build) in the menu's Portuguese.
const char* Pt(const char* en) {
  static constexpr std::pair<const char*, const char*> kWords[] = {
      {"PLAYER", "JOGADOR"},
      {"WEAPONS", "ARMAS"},
      {"VEHICLE", "VEÍCULO"},
      {"MAP", "MAPA"},
      {"DISPLAY", "TELA"},
      {"CONTROLLER", "CONTROLE"},
      {"Infinite Health", "Vida Infinita"},
      {"Movement Speed", "Velocidade de Movimento"},
      {"Jump Height", "Altura do Pulo"},
      {"No Fall Damage", "Sem Dano de Queda"},
      {"Infinite Ammo", "Munição Infinita"},
      {"Infinite Vehicle Health", "Veículo Indestrutível"},
      {"Vehicle Speed", "Velocidade do Veículo"},
      {"Agility Orbs on Map", "Orbes de Agilidade no Mapa"},
      {"Hidden Orbs on Map", "Orbes Ocultos no Mapa"},
      {"FPS Counter", "Contador de FPS"},
      {"DualSense Features", "Recursos do DualSense"},
      {"OFF", "DESLIGADO"},
      {"ON", "LIGADO"},
      {"MINIMAP", "MINIMAPA"},
      {"MINIMAP + WORLD", "MINIMAPA + MUNDO"},
      {"SIMPLE", "SIMPLES"},
      {"DETAILED", "DETALHADO"},
  };
  if (!en || !Portuguese()) return en;
  for (const auto& w : kWords)
    if (std::strcmp(w.first, en) == 0) return w.second;
  return en;
}

struct Row {
  const char* section;  // a heading over the row, or nullptr
  const char* label;
  int cheat;  // the cheats' index, or -1
};

std::vector<Row> Rows() {
  std::vector<Row> rows;
  const char* last = nullptr;
#if CD_QUICK_MENU_CD2
  for (int i = 0; i < int(cheats2::Cheat::kCount); ++i) {
    const char* section = cheats2::Section(cheats2::Cheat(i));
    rows.push_back({section != last ? section : nullptr, cheats2::Label(cheats2::Cheat(i)), i});
    last = section;
  }
#else
  for (size_t i = 0; i < cheats::kCheatCount; ++i) {
    const cheats::CheatInfo& info = cheats::kCheats[i];
    if (!cheats::Supported(info.cheat)) continue;
    const char* section = cheats::SectionLabel(info.section);
    rows.push_back({section != last ? section : nullptr, info.label, int(i)});
    last = section;
  }
#endif
  rows.push_back({"DISPLAY", "FPS Counter", -1});
  rows.push_back({"CONTROLLER", "DualSense Features", -2});
  return rows;
}

int CounterMode() {
  const std::string m = REXCVAR_GET(crackdown_fps_counter);
  return m == "detailed" ? 2 : m == "simple" ? 1 : 0;
}

std::string ValueOf(const Row& row) {
  if (row.cheat >= 0) {
#if CD_QUICK_MENU_CD2
    return Pt(cheats2::On(cheats2::Cheat(row.cheat)) ? "ON" : "OFF");
#else
    const cheats::CheatInfo& info = cheats::kCheats[size_t(row.cheat)];
    std::string v = cheats::ValueLabel(info.kind, cheats::Value(info.cheat));
    for (char& c : v) c = char(std::toupper(static_cast<unsigned char>(c)));
    return Pt(v.c_str());
#endif
  }
  if (row.cheat == -1) return Pt(CounterMode() == 2 ? "DETAILED" : CounterMode() == 1 ? "SIMPLE" : "OFF");
  return Pt(rex::input::sony::GetOptions().enabled ? "ON" : "OFF");
}

// One step forward (or back) of the row's value.
void Change(const Row& row, bool back) {
  if (row.cheat >= 0) {
#if CD_QUICK_MENU_CD2
    (void)back;  // (a toggle: either way flips it)
    cheats2::Toggle(cheats2::Cheat(row.cheat));
    cheats2::Save(kCheatsFile);
#else
    const cheats::CheatInfo& info = cheats::kCheats[size_t(row.cheat)];
    for (size_t n = back ? info.values.size() - 1 : 1; n; --n) cheats::Cycle(info.cheat);
    crackdown::cheats_ps5::Save(kCheatsFile);
#endif
    REXLOG_INFO("Quick menu: {} {}", row.label, ValueOf(row));  // (the checklist's proof of each cheat)
  } else if (row.cheat == -1) {
    static constexpr const char* kModes[] = {"off", "simple", "detailed"};
    const int mode = (CounterMode() + (back ? 2 : 1)) % 3;
    rex::cvar::SetFlagByName("crackdown_fps_counter", kModes[mode]);
    SetMenuValues("fps_counter", mode);
  } else {
    const bool on = !rex::input::sony::GetOptions().enabled;
    rex::input::sony::SetFeaturesEnabled(on);
    SetMenuValues("dualsense", on ? 1 : 0);
  }
}

constexpr ImU32 kWhite = IM_COL32(236, 232, 226, 255), kGrey = IM_COL32(150, 146, 140, 255),
                kBlue = IM_COL32(70, 165, 255, 255);

ImU32 Fade(ImU32 c, float a) { return (c & 0x00FFFFFFu) | (ImU32(float(c >> 24) * a) << 24); }

}  // namespace

QuickMenu::QuickMenu(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

bool QuickMenu::WantsContinuousRepaint() const { return g_open.load(std::memory_order_relaxed) || shown_ > 0; }

void QuickMenu::OnDraw(ImGuiIO& io) {
  const bool open = g_open.load(std::memory_order_acquire);
  shown_ = std::clamp(shown_ + (open ? 1.0f : -1.0f) * std::min(io.DeltaTime, 0.1f) * 8.0f, 0.0f, 1.0f);
  const std::vector<Row> rows = Rows();
  for (uint8_t e; (e = Take());) {
    if (e == kNavUp) row_ = (row_ + int(rows.size()) - 1) % int(rows.size());
    if (e == kNavDown) row_ = (row_ + 1) % int(rows.size());
    if (e == kNavLeft) Change(rows[size_t(row_)], true);
    if (e == kNavRight || e == kNavSelect) Change(rows[size_t(row_)], false);
  }
  if (shown_ <= 0) return;

  const ImVec2 size = io.DisplaySize;
  const float u = std::min(size.x / 1920.0f, size.y / 1080.0f), a = shown_;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImFont* item = MenuItemFont() ? MenuItemFont() : ImGui::GetFont();
  ImFont* body = MenuBodyFont() ? MenuBodyFont() : ImGui::GetFont();
  int headings = 0;
  for (const Row& r : rows) headings += r.section ? 1 : 0;
  const float row_h = 40 * u, head_h = 44 * u, w = 760 * u;
  const float h = 110 * u + rows.size() * row_h + headings * head_h + 70 * u;
  const float x = (size.x - w) * 0.5f, y0 = (size.y - h) * 0.5f;
  dl->AddRectFilled(ImVec2(0, 0), size, Fade(IM_COL32(0, 0, 0, 110), a));
  dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + w, y0 + h), Fade(IM_COL32(8, 10, 12, 235), a), 10 * u);
  dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + 8 * u, y0 + h), Fade(kBlue, a), 10 * u, ImDrawFlags_RoundCornersLeft);
  dl->AddText(item, 46 * u, ImVec2(x + 40 * u, y0 + 26 * u), Fade(kWhite, a), T("QUICK MENU", "MENU RÁPIDO"));
#if CD_QUICK_MENU_CD2
  if (!cheats2::Available())
    dl->AddText(body, 20 * u, ImVec2(x + 330 * u, y0 + 44 * u), Fade(kGrey, a),
                T("(the cheats act once you're playing)", "(as trapaças valem quando você estiver jogando)"));
#else
  if (!cheats::Available())
    dl->AddText(body, 20 * u, ImVec2(x + 330 * u, y0 + 44 * u), Fade(kGrey, a),
                T("(the cheats act once you're playing solo)", "(as trapaças valem quando você joga sozinho)"));
#endif
  float y = y0 + 100 * u;
  for (size_t i = 0; i < rows.size(); ++i) {
    const Row& r = rows[i];
    if (r.section) {
      y += 10 * u;
      dl->AddText(body, 20 * u, ImVec2(x + 40 * u, y), Fade(kBlue, a), Pt(r.section));
      dl->AddRectFilled(ImVec2(x + 40 * u, y + 28 * u), ImVec2(x + w - 40 * u, y + 29 * u),
                        Fade(IM_COL32(255, 255, 255, 40), a));
      y += head_h - 10 * u;
    }
    const bool sel = int(i) == row_;
    if (sel)
      dl->AddRectFilled(ImVec2(x + 24 * u, y + 2 * u), ImVec2(x + w - 24 * u, y + row_h - 2 * u),
                        Fade(IM_COL32(70, 165, 255, 40), a), 4 * u);
    dl->AddText(item, 28 * u, ImVec2(x + 40 * u, y + 5 * u), Fade(sel ? kWhite : kGrey, a), Pt(r.label));
    const std::string v = sel ? "<  " + ValueOf(r) + "  >" : ValueOf(r);
    const float vw = item->CalcTextSizeA(28 * u, FLT_MAX, 0, v.c_str()).x;
    dl->AddText(item, 28 * u, ImVec2(x + w - 40 * u - vw, y + 5 * u), Fade(sel ? kBlue : kGrey, a), v.c_str());
    y += row_h;
  }
  dl->AddText(body, 20 * u, ImVec2(x + 40 * u, y0 + h - 46 * u), Fade(kGrey, a),
              T("UP DOWN  MOVE      LEFT RIGHT / X  CHANGE      O / L3+R3  CLOSE",
                "CIMA BAIXO  MOVER      ESQ. DIR. / X  MUDAR      O / L3+R3  FECHAR"));
}

}  // namespace cdps5

// The title's XInputGetState: r3 the user, r4 the state; returns 0 when read.
#if CD_QUICK_MENU_CD2
REX_EXTERN(__imp__sub_829551D8);
REX_HOOK_RAW(sub_829551D8) {
  const uint32_t user = ctx.r3.u32, state = ctx.r4.u32;
  __imp__sub_829551D8(ctx, base);
  if (user == 0 && state && ctx.r3.u32 == 0) cdps5::Filter(base, state);
}
#else
REX_EXTERN(__imp__sub_827CCB18);
REX_HOOK_RAW(sub_827CCB18) {
  const uint32_t user = ctx.r3.u32, state = ctx.r4.u32;
  __imp__sub_827CCB18(ctx, base);
  if (user == 0 && state && ctx.r3.u32 == 0) cdps5::Filter(base, state);
}
#endif

#endif  // REX_PLATFORM_PS5
