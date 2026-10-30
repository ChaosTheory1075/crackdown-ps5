// Crackdown PS5 - the menu (see crackdown_menu.h): shown before the game is
// set up (CrackdownApp::OnFinalizePaths, asynchronous), driven by the DualSense through
// scePad, the menu song looping under it. L1/R1 change the page.
#include "crackdown_menu.h"
#if REX_PLATFORM_PS5

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>

#include "cd_install.h"
#include "cd_trophies.h"
#include "game/sound.h"
#include "imgui.h"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

extern "C" {
int sceUserServiceInitialize(void* params);
int sceUserServiceGetInitialUser(int32_t* user_id);
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t type, int32_t index, const void* param);
int scePadGetHandle(int32_t user_id, int32_t type, int32_t index);
int scePadReadState(int32_t handle, void* data);
int sceSystemServiceParamGetInt(int param_id, int* value);
}

// The fonts, built into the app (gta4-ps5's ps5/assets/fonts: Oswald and
// Inter, SIL Open Font License).
#define CD_INCBIN(name, file)                                                        \
  __asm__(".section .rodata\n.global " #name "\n.balign 16\n" #name ":\n.incbin \"" \
          file "\"\n.global " #name "_end\n" #name "_end:\n.byte 0\n.previous\n");    \
  extern "C" const unsigned char name[];                                              \
  extern "C" const unsigned char name##_end[];
CD_INCBIN(cdps5_font_title, CDPS5_FONTS_DIR "/oswald-latin-700-normal.ttf")
CD_INCBIN(cdps5_font_item, CDPS5_FONTS_DIR "/oswald-latin-600-normal.ttf")
CD_INCBIN(cdps5_font_body, CDPS5_FONTS_DIR "/inter-latin-400-normal.ttf")

namespace cdps5 {
namespace {

namespace fs = std::filesystem;

ImFont* g_title = nullptr;
ImFont* g_item = nullptr;
ImFont* g_body = nullptr;

// The menu's pictures: kept for the app's life (the frame PLAY was pressed on
// is rendered after the dialog has gone).
std::vector<std::unique_ptr<rex::ui::ImmediateTexture>>* g_textures =
    new std::vector<std::unique_ptr<rex::ui::ImmediateTexture>>();

// ---- the pad ----

enum : uint32_t {
  kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kCross = 16, kCircle = 32, kL1 = 64, kR1 = 128, kTriangle = 256
};

constexpr uint32_t kPadUp = 0x00000010u, kPadRight = 0x00000020u, kPadDown = 0x00000040u,
                   kPadLeft = 0x00000080u, kPadL1 = 0x00000400u, kPadR1 = 0x00000800u,
                   kPadTriangle = 0x00001000u, kPadCircle = 0x00002000u, kPadCross = 0x00004000u;
struct PadData {
  uint32_t buttons;
  uint8_t lx, ly, rx, ry, l2, r2;
  uint16_t padding;
  uint8_t rest[240];
};

// (a handle of our own: the input driver later gets the same one)
class Pad {
 public:
  // The buttons pressed since the last read (held directions repeat).
  uint32_t Pressed() {
    if (handle_ < 0) {
      sceUserServiceInitialize(nullptr);
      scePadInit();
      int32_t user = -1;
      if (sceUserServiceGetInitialUser(&user) == 0 && user >= 0) {
        handle_ = scePadOpen(user, 0, 0, nullptr);
        if (handle_ < 0) handle_ = scePadGetHandle(user, 0, 0);
      }
      if (handle_ < 0) return 0;
    }
    PadData d{};
    if (scePadReadState(handle_, &d) != 0) return 0;
    uint32_t b = 0;
    if ((d.buttons & kPadUp) || d.ly < 64) b |= kUp;
    if ((d.buttons & kPadDown) || d.ly > 192) b |= kDown;
    if ((d.buttons & kPadLeft) || d.lx < 64) b |= kLeft;
    if ((d.buttons & kPadRight) || d.lx > 192) b |= kRight;
    if (d.buttons & kPadCross) b |= kCross;
    if (d.buttons & kPadCircle) b |= kCircle;
    if (d.buttons & kPadTriangle) b |= kTriangle;
    if (d.buttons & kPadL1) b |= kL1;
    if (d.buttons & kPadR1) b |= kR1;
    uint32_t pressed = b & ~last_;
    const auto now = std::chrono::steady_clock::now();
    constexpr uint32_t kDirs = kUp | kDown | kLeft | kRight;
    if (b & kDirs) {
      if ((b & kDirs) != (last_ & kDirs)) {
        repeat_at_ = now + std::chrono::milliseconds(350);
      } else if (now >= repeat_at_) {
        pressed |= b & kDirs;
        repeat_at_ = now + std::chrono::milliseconds(90);
      }
    }
    last_ = b;
    // (the button that started the app is still down on the first read)
    if (first_) {
      first_ = false;
      return 0;
    }
    return pressed;
  }

 private:
  int32_t handle_ = -1;
  uint32_t last_ = 0;
  bool first_ = true;
  std::chrono::steady_clock::time_point repeat_at_{};
};

// ---- the settings (app/menu.ini: key=value lines) ----

// The game's languages: the 13 text folders the disc has (game/Local; Title Update 6 adds none),
// by the number the title gives them (crackdown_locale, locale_ps5.cpp), with the Xbox 360
// language the console is said to have for each (XGetLanguage, XCONFIG_USER_LANGUAGE). AUTO is
// the PS5's own language.
struct Language {
  const char* name;
  int locale;  // -1: AUTO
  int xbox;
};
constexpr Language kLanguages[] = {
    {"AUTO", -1, 0},       {"ENGLISH", 0, 1},  {"FRENCH", 1, 4},     {"GERMAN", 3, 3},
    {"ITALIAN", 2, 6},     {"SPANISH", 4, 5},  {"SPANISH (LATAM)", 5, 5},
    {"POLISH", 7, 1},      {"CZECH", 6, 1},    {"HUNGARIAN", 8, 1},  {"SLOVAK", 13, 1},
    {"JAPANESE", 9, 2},    {"KOREAN", 10, 7},  {"CHINESE", 11, 8}};
constexpr int kLanguageCount = int(sizeof(kLanguages) / sizeof(kLanguages[0]));

// kLanguages' row for a text folder's number (AUTO for none of them).
int LanguageIndex(int locale) {
  for (int i = 1; i < kLanguageCount; ++i)
    if (kLanguages[i].locale == locale) return i;
  return 0;
}

// The PS5's language as kLanguages' row; English for one the game doesn't have.
int SystemLanguage() {
  int v = 1;  // (English, US)
  if (sceSystemServiceParamGetInt(1 /* SCE_SYSTEM_SERVICE_PARAM_ID_LANG */, &v) != 0) v = 1;
  switch (v) {
    case 0: return LanguageIndex(9);             // Japanese
    case 2: case 22: return LanguageIndex(1);    // French, French (Canada)
    case 3: return LanguageIndex(4);             // Spanish
    case 20: return LanguageIndex(5);            // Spanish (Latin America)
    case 4: return LanguageIndex(3);             // German
    case 5: return LanguageIndex(2);             // Italian
    case 9: return LanguageIndex(10);            // Korean
    case 10: case 11: return LanguageIndex(11);  // Chinese (traditional, simplified)
    case 16: return LanguageIndex(7);            // Polish
    case 23: return LanguageIndex(6);            // Czech
    case 24: return LanguageIndex(8);            // Hungarian
    default: return LanguageIndex(0);
  }
}

struct Settings {
  int language = 0;      // kLanguages: AUTO, or the text folder the game is given (crackdown_locale)
  int fps = 60;        // crackdown_frame_rate: 60 (the title's variable step) or 30 (locked)
  bool fxaa = false;     // swap_post_effect: none or fxaa
  int filter = 3;        // anisotropic_override 2..5: 2x, 4x, 8x, 16x
  bool lane = true;      // the GPU's second lane (crackdown_gpu_lane)
  bool refresh = false;  // clear_memory_page_state: every page re-read from the game each frame
  int music = 60;        // the menu song's level, 0..100

  static constexpr const char* kFilterNames[] = {"", "", "2X", "4X", "8X", "16X"};

  static Settings Load(const fs::path& app) {
    Settings s;
    if (std::FILE* f = std::fopen((app / "menu.ini").string().c_str(), "rb")) {
      char line[256];
      while (std::fgets(line, sizeof(line), f)) {
        char key[64] = {};
        int value = 0;
        if (std::sscanf(line, " %63[a-z_] = %d", key, &value) != 2) continue;
        const std::string k(key);
        if (k == "game_language") s.language = LanguageIndex(value);
        if (k == "frame_rate") s.fps = value == 30 ? 30 : 60;
        if (k == "fxaa") s.fxaa = value != 0;
        if (k == "texture_filtering") s.filter = std::clamp(value, 2, 5);
        if (k == "menu_music") s.music = std::clamp(value, 0, 100);
        if (k == "gpu_lane") s.lane = value != 0;
        if (k == "memory_refresh") s.refresh = value != 0;
      }
      std::fclose(f);
    }
    return s;
  }

  void Save(const fs::path& app) const {
    if (std::FILE* f = std::fopen((app / "menu.ini").string().c_str(), "wb")) {
      std::fprintf(f, "game_language = %d\nframe_rate = %d\nfxaa = %d\ntexture_filtering = %d\ngpu_lane = %d\n"
                   "memory_refresh = %d\nmenu_music = %d\n", kLanguages[language].locale, fps, fxaa ? 1 : 0, filter,
                   lane ? 1 : 0, refresh ? 1 : 0, music);
      std::fclose(f);
    }
  }

  // The cvars they set, for this start.
  void Apply() const {
    // (the Xbox 360's own resolution, always: 2x cost the PS5 far more than it showed)
    rex::cvar::SetFlagByName("draw_resolution_scale_x", "1");
    rex::cvar::SetFlagByName("draw_resolution_scale_y", "1");
    rex::cvar::SetFlagByName("crackdown_frame_rate", std::to_string(fps));
    rex::cvar::SetFlagByName("swap_post_effect", fxaa ? "fxaa" : "none");
    rex::cvar::SetFlagByName("anisotropic_override", std::to_string(filter));
    rex::cvar::SetFlagByName("crackdown_gpu_lane", lane ? "true" : "false");
    // (off: the pages the GPU read stay valid until the game writes them - as Xenia's own default; on,
    // every one of them was copied again each frame, 93% of them unchanged in crackdown_003.log)
    rex::cvar::SetFlagByName("clear_memory_page_state", refresh ? "true" : "false");
    const Language& l = kLanguages[language ? language : SystemLanguage()];
    rex::cvar::SetFlagByName("crackdown_locale", std::to_string(l.locale));
    rex::cvar::SetFlagByName("user_language", std::to_string(l.xbox));
    REXLOG_INFO("Crackdown menu: language {}{} (text folder {}, Xbox 360 language {}), {} fps, anti-aliasing {}, "
                "texture filtering {}, the GPU's second lane {}, memory refresh {}", l.name,
                language ? "" : " (AUTO)", l.locale, l.xbox, fps, fxaa ? "FXAA" : "off", kFilterNames[filter], lane ? "on" : "off",
                refresh ? "every frame" : "off");
  }
};

// ---- the look ----

constexpr ImU32 kWhite = IM_COL32(236, 232, 226, 255);
constexpr ImU32 kGrey = IM_COL32(150, 146, 140, 255);
constexpr ImU32 kDim = IM_COL32(90, 88, 84, 255);
constexpr ImU32 kGreen = IM_COL32(120, 220, 40, 255);  // (the Agency's green)

struct Image {
  ImTextureID id = 0;
  float w = 0, h = 0;
  explicit operator bool() const { return id != 0; }
};

float Width(ImFont* f, float size, const std::string& s) { return f->CalcTextSizeA(size, FLT_MAX, 0, s.c_str()).x; }

// `s` cut to `width` (with "..."), in whole UTF-8 letters.
std::string Fit(ImFont* f, float size, std::string s, float width) {
  if (Width(f, size, s) <= width) return s;
  while (!s.empty() && Width(f, size, s + "...") > width) {
    s.pop_back();
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
  }
  return s + "...";
}

std::string Date(int64_t t, const char* format) {
  const std::time_t tt = std::time_t(t);
  char out[64] = {};
  if (const std::tm* tm = std::localtime(&tt)) std::strftime(out, sizeof(out), format, tm);
  return out;
}

std::string Upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// What's in the app's game/ and dlc/ folders (names in any case: the disc has Default.xex).
struct GameFiles {
  bool xex = false, xexp = false, update = false;
  int dlc = 0;

  static GameFiles Find(const fs::path& app) {
    GameFiles f;
    std::error_code ec;
    for (fs::directory_iterator it(app / "game", ec), end; !ec && it != end; it.increment(ec)) {
      const std::string name = Upper(it->path().filename().string());
      if (name == "DEFAULT.XEX" && it->is_regular_file(ec)) f.xex = true;
      if (name == "DEFAULT.XEXP" && it->is_regular_file(ec)) f.xexp = true;
      if (name == "UPDATE" && it->is_directory(ec)) f.update = true;
    }
    ec.clear();
    for (fs::directory_iterator it(app / "dlc", ec), end; !ec && it != end; it.increment(ec))
      if (it->is_directory(ec)) ++f.dlc;
    return f;
  }
  bool Ready() const { return xex && xexp; }
};

// The pages, in the tab bar's order.
enum Page { kHome, kContent, kSaves, kSettings, kTrophies, kAbout, kPages };
const char* const kPageNames[kPages] = {"HOME", "CONTENT", "SAVES", "SETTINGS", "TROPHIES", "ABOUT"};

// An ember rising from the city.
struct Ember {
  float x = 0, y = 0, speed = 0, drift = 0, size = 0, life = 0, lifetime = 1, phase = 0;
};

class MenuDialog final : public rex::ui::ImGuiDialog {
 public:
  MenuDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, const fs::path& app,
             std::function<void()> play)
      : ImGuiDialog(drawer), immediate_(immediate), app_(app), play_(std::move(play)),
        settings_(Settings::Load(app)) {
    const fs::path hub = app / "hub";
    for (const char* name : {"background.jpg", "background.png"})
      if (!background_) background_ = Load(hub / name);
    logo_ = Load(hub / "logo.png");
    StartMusic(hub / "music");
    // (an install the app was closed during: not to be taken for a finished one)
    {
      std::error_code ec;
      fs::remove_all(app / "game.installing", ec);
      fs::remove_all(app / "update.installing", ec);
      for (fs::directory_iterator it(app / "dlc", ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".installing") fs::remove_all(it->path(), ec);
    }
    Refresh();
    where_ = install::Where(app);
    REXLOG_INFO("Crackdown menu: game {}, Title Update 6 {}, its other files {}, {} DLC packs",
                files_.xex ? "found" : "MISSING", files_.xexp ? "found" : "MISSING",
                files_.update ? "found" : "missing", files_.dlc);
    // (the runtime's user data: /app0/UserData/crackdown)
    user_data_ = app / "UserData" / "crackdown";
    TakeTrophies(trophies::Load(user_data_));
    REXLOG_INFO("Crackdown menu: trophies {} ({} of {} earned)", trophies_.loaded ? "listed" : "not listed yet",
                trophies_.earned, trophies_.earnable);
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (done_) return;
    const float dt = std::min(io.DeltaTime, 0.1f);
    time_ += dt;
    pictures_this_frame_ = 0;
    HandlePad(pad_.Pressed());
    TickInstaller();
    if (music_) music_->SetVolume(leaving_ ? 0.0f : settings_.music / 100.0f);
    tab_x_ += (tab_target_x_ - tab_x_) * std::min(1.0f, dt * 14.0f);
    tab_w_ += (tab_target_w_ - tab_w_) * std::min(1.0f, dt * 14.0f);
    page_in_ = std::min(1.0f, page_in_ + dt * 5.0f);
    Draw(io.DisplaySize, dt);
    if (leaving_ && (leave_t_ += dt) > 0.7f) {
      done_ = true;
      settings_.Save(app_);
      settings_.Apply();
      Close();
      play_();
    }
  }

 private:
  enum SettingsRow { kLanguageRow, kFrameRate, kAntiAliasing, kFiltering, kLane, kRefresh, kMusic, kSettingsRows };

  // ---- the installer (cd_install.h) ----

  // `s` with the install folder's real place (any drive, any name) for "PPSA53070/install/".
  std::string Here(std::string s) const {
    for (const std::pair<std::string, std::string>& swap :
         {std::pair<std::string, std::string>{"PPSA53070/install/", where_ + "/"},
          std::pair<std::string, std::string>{"PPSA53070/game/", "the app's game folder"}})
      for (size_t at; (at = s.find(swap.first)) != std::string::npos;) s.replace(at, swap.first.size(), swap.second);
    return s;
  }

  void Refresh() {
    files_ = GameFiles::Find(app_);
    installed_ = install::Check(app_);
    sources_ = install::Scan(app_);
    // (an imported save's file deleted: its row gone, the selection kept on the list)
    saves_row_ = std::min(saves_row_, int(SaveSources().size()));
  }

  // What waits in install/ for a content row (0 the game, 1 TU6, 2.. the DLC); nullptr: nothing.
  const install::Source* SourceFor(int row) const {
    for (const auto& s : sources_) {
      if (row == 0 && (s.kind == install::Kind::kGameImage || s.kind == install::Kind::kGameFolder)) return &s;
      if (row == 1 && (s.kind == install::Kind::kUpdate || s.kind == install::Kind::kOtherUpdate)) return &s;
      if (row >= 2 && s.kind == install::Kind::kDlc && s.dlc == row - 2) return &s;
    }
    return nullptr;
  }
  bool RowInstalled(int row) const {
    return row == 0 ? installed_.game : row == 1 ? installed_.update : installed_.dlc[row - 2];
  }
  // The saves page's rows after BACK UP: the saves and backups to import or restore.
  std::vector<const install::Source*> SaveSources() const {
    std::vector<const install::Source*> out;
    for (const auto& s : sources_)
      if (s.kind == install::Kind::kSave || s.kind == install::Kind::kBackup) out.push_back(&s);
    return out;
  }

  // Once a frame: a finished job's outcome, and the next of what installs by itself (what
  // isn't installed yet: the game, Title Update 6, the DLC), one at a time.
  void TickInstaller() {
    auto& job = install::TheJob();
    if (job.Generation() != job_generation_) {
      job_generation_ = job.Generation();
      result_t_ = time_;
      Refresh();
    }
    // (not in the first seconds: the menu on screen first, the progress with it)
    if (job.Busy() || leaving_ || time_ < 2.0f) return;
    for (const auto& s : sources_) {
      if (!install::Automatic(s.kind) || tried_.count(s.path.string())) continue;
      const int row = s.kind == install::Kind::kUpdate ? 1 : s.kind == install::Kind::kDlc ? 2 + s.dlc : 0;
      if (RowInstalled(row)) continue;
      tried_.insert(s.path.string());
      job.Install(app_, s);
      return;
    }
    // Everything installed: whether to delete what it was installed from (asked once).
    if (!asking_ && !job.Leftovers().empty()) {
      asking_ = true;
      ask_files_.clear();
      uint64_t bytes = 0;
      for (const fs::path& f : job.Leftovers()) {
        std::error_code ec;
        const uint64_t n = fs::is_directory(f, ec) ? 0 : fs::file_size(f, ec);
        if (!ec) bytes += n;
        ask_files_.push_back(f.filename().string());
      }
      char gb[32];
      std::snprintf(gb, sizeof(gb), "%.1f GB", double(bytes) / double(1ull << 30));
      ask_size_ = gb;
    }
  }

  // A second press of the same button on the same row within 3 s: the go-ahead.
  bool Confirmed(int key) {
    if (confirm_key_ == key && time_ - confirm_t_ < 3.0f) {
      confirm_key_ = -1;
      return true;
    }
    confirm_key_ = key;
    confirm_t_ = time_;
    return false;
  }
  bool Confirming(int key) const { return confirm_key_ == key && time_ - confirm_t_ < 3.0f; }

  // ---- input ----

  void HandlePad(uint32_t p) {
    if (leaving_ || !p) return;
    if (asking_) {
      // (the question about the installed files: X deletes them, O keeps them)
      if (p & kCross) install::TheJob().DeleteLeftovers();
      if (p & kCircle) install::TheJob().ForgetLeftovers();
      if (p & (kCross | kCircle)) asking_ = false;
      return;
    }
    if (p & (kL1 | kR1)) {
      page_ = Page((page_ + ((p & kR1) ? 1 : kPages - 1)) % kPages);
      page_in_ = 0;
      return;
    }
    if ((p & kCircle) && page_ != kHome) {
      page_ = kHome;
      page_in_ = 0;
      return;
    }
    switch (page_) {
      case kHome:
        if ((p & kCross) && install::TheJob().Busy()) break;  // (PLAY waits for the install)
        if ((p & kCross) && !files_.Ready()) {
          Refresh();  // (copied in since the menu opened?)
          missing_flash_ = 1.0f;
        }
        if ((p & kCross) && files_.Ready()) {
          leaving_ = true;
          if (music_) music_->FadeOut(0.6f);
        }
        break;
      case kContent: {
        if (p & kUp) content_row_ = (content_row_ + 3) % 4;
        if (p & kDown) content_row_ = (content_row_ + 1) % 4;
        auto& job = install::TheJob();
        if (job.Busy()) break;
        if (p & kCross) {
          Refresh();
          if (const install::Source* s = SourceFor(content_row_)) job.Install(app_, *s);
        }
        if ((p & kTriangle) && RowInstalled(content_row_) && Confirmed(100 + content_row_))
          job.Uninstall(app_, install::Remove(content_row_));
        break;
      }
      case kSaves: {
        const int rows = 1 + int(SaveSources().size());
        if (p & kUp) saves_row_ = (saves_row_ + rows - 1) % rows;
        if (p & kDown) saves_row_ = (saves_row_ + 1) % rows;
        saves_row_ = std::min(saves_row_, rows - 1);
        auto& job = install::TheJob();
        if (job.Busy() || !(p & kCross)) break;
        if (saves_row_ == 0) {
          job.Backup(app_);
        } else if (Confirmed(200 + saves_row_)) {
          job.Install(app_, *SaveSources()[size_t(saves_row_ - 1)]);
        }
        break;
      }
      case kSettings: {
        if (p & kUp) row_ = (row_ + kSettingsRows - 1) % kSettingsRows;
        if (p & kDown) row_ = (row_ + 1) % kSettingsRows;
        const int step = (p & kRight) ? 1 : (p & kLeft) ? -1 : 0;
        if (row_ == kLanguageRow)
          settings_.language = (settings_.language + ((p & kCross) ? 1 : step) + kLanguageCount) % kLanguageCount;
        if (row_ == kFrameRate && (step || (p & kCross))) settings_.fps = settings_.fps == 60 ? 30 : 60;
        if (row_ == kAntiAliasing && (step || (p & kCross))) settings_.fxaa = !settings_.fxaa;
        if (row_ == kFiltering) {
          if (p & kCross) settings_.filter = settings_.filter == 5 ? 2 : settings_.filter + 1;
          settings_.filter = std::clamp(settings_.filter + step, 2, 5);
        }
        if (row_ == kLane && (step || (p & kCross))) settings_.lane = !settings_.lane;
        if (row_ == kRefresh && (step || (p & kCross))) settings_.refresh = !settings_.refresh;
        if (row_ == kMusic) settings_.music = std::clamp(settings_.music + step * 10, 0, 100);
        break;
      }
      case kTrophies: {
        const int n = int(items_.size());
        if (!n) break;
        if (p & kUp) trophy_ = (trophy_ + n - 1) % n;
        if (p & kDown) trophy_ = (trophy_ + 1) % n;
        if (p & kLeft) trophy_ = std::max(0, trophy_ - 8);
        if (p & kRight) trophy_ = std::min(n - 1, trophy_ + 8);
        if (p & (kUp | kDown | kLeft | kRight)) detail_in_ = 0;
        break;
      }
      default:
        break;
    }
  }

  // ---- the trophies ----

  // A row of the list: a trophy (its index; -1: the platinum), in its group (0 earned, 1 not yet).
  struct Item {
    int index = 0, group = 0;
  };

  void TakeTrophies(trophies::List list) {
    trophies_ = std::move(list);
    items_.clear();
    if (!trophies_.loaded) return;
    const auto& all = trophies_.trophies;
    std::vector<Item> earned, rest;
    for (int i = 0; i < int(all.size()); ++i) (all[size_t(i)].earned ? earned : rest).push_back({i, all[size_t(i)].earned ? 0 : 1});
    std::stable_sort(earned.begin(), earned.end(), [&](const Item& a, const Item& b) {
      return all[size_t(a.index)].earned > all[size_t(b.index)].earned;
    });
    const bool plat = trophies_.Platinum();
    if (plat) items_.push_back({-1, 0});
    items_.insert(items_.end(), earned.begin(), earned.end());
    if (!plat) items_.push_back({-1, 1});
    items_.insert(items_.end(), rest.begin(), rest.end());
  }

  bool Earned(const Item& it) const {
    return it.index < 0 ? trophies_.Platinum() : trophies_.trophies[size_t(it.index)].earned != 0;
  }
  trophies::Grade GradeOf(const Item& it) const {
    return it.index < 0 ? trophies::Grade::kPlatinum : trophies_.trophies[size_t(it.index)].grade();
  }
  std::string NameOf(const Item& it) const {
    if (it.index < 0) return "Crackdown Platinum";
    const auto& t = trophies_.trophies[size_t(it.index)];
    return (!t.earned && t.hidden()) ? "Hidden trophy" : t.name;
  }
  std::string TextOf(const Item& it) const {
    if (it.index < 0) return "Earn every other Crackdown trophy (" + std::to_string(trophies_.earnable) + ").";
    const auto& t = trophies_.trophies[size_t(it.index)];
    if (!t.earned && t.hidden()) return "Keep playing to find out what this one is.";
    return t.earned || t.locked_description.empty() ? t.description : t.locked_description;
  }

  // A trophy's picture (the platinum's: the game's own), loaded the first time it's drawn
  // (two a frame at most, so the page doesn't hold the menu up).
  Image Picture(const Item& it) {
    const uint32_t key = it.index < 0 ? 0 : std::max(1u, trophies_.trophies[size_t(it.index)].image_id);
    if (auto found = pictures_.find(key); found != pictures_.end()) return found->second;
    if (pictures_this_frame_ >= 2) return {};
    ++pictures_this_frame_;
    const Image img = Load(it.index < 0 ? trophies::TitleIconFile(user_data_)
                                        : trophies::IconFile(user_data_, trophies_.trophies[size_t(it.index)].image_id));
    pictures_[key] = img;
    return img;
  }

  // The picture in a square: dimmed until it's earned, with a padlock; a question mark for a hidden one.
  void Icon(ImDrawList* dl, const Item& it, float x, float y, float size, float a) {
    const bool earned = Earned(it);
    const trophies::Trophy* t = it.index < 0 ? nullptr : &trophies_.trophies[size_t(it.index)];
    const float r = size * 0.08f;
    if (t && !earned && t->hidden()) {
      dl->AddRectFilled(ImVec2(x, y), ImVec2(x + size, y + size), IM_COL32(38, 40, 46, int(255 * a)), r);
      const char* q = "?";
      const float qs = size * 0.6f;
      dl->AddText(g_item, qs, ImVec2(x + (size - Width(g_item, qs, q)) * 0.5f, y + size * 0.16f),
                  IM_COL32(255, 255, 255, int(128 * a)), q);
      return;
    }
    const Image img = Picture(it);
    if (img) {
      const int tint = earned ? 255 : 90;
      dl->AddImageRounded(img.id, ImVec2(x, y), ImVec2(x + size, y + size), ImVec2(0, 0), ImVec2(1, 1),
                          IM_COL32(tint, tint, tint, int(255 * a)), r);
    } else {
      dl->AddRectFilled(ImVec2(x, y), ImVec2(x + size, y + size), IM_COL32(38, 40, 46, int(255 * a)), r);
      trophies::DrawCup(dl, ImVec2(x + size * 0.5f, y + size * 0.5f), size * 0.56f, GradeOf(it), a, earned);
    }
    if (!earned) {  // a padlock in the corner
      const float k = size / 64.0f, lx = x + size - 12 * k, ly = y + size - 12 * k;
      dl->AddCircleFilled(ImVec2(lx, ly), 12 * k, IM_COL32(0, 0, 0, int(190 * a)), 20);
      dl->AddRectFilled(ImVec2(lx - 5 * k, ly - 1.5f * k), ImVec2(lx + 5 * k, ly + 6 * k),
                        IM_COL32(230, 230, 230, int(220 * a)), 1.5f * k);
      dl->PathClear();
      dl->PathArcTo(ImVec2(lx, ly - 2 * k), 3.6f * k, 3.14159f, 6.28318f, 12);
      dl->PathStroke(IM_COL32(230, 230, 230, int(220 * a)), 0, 1.8f * k);
    }
  }

  // ---- drawing ----

  void Draw(const ImVec2& size, float dt) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float u = size.y / 1080.0f;  // (laid out for 1080p, scaled to the screen)
    const float fade = leaving_ ? std::clamp(1.0f - leave_t_ / 0.6f, 0.0f, 1.0f) : std::min(time_ / 0.8f, 1.0f);
    fade_ = fade;
    u_ = u;

    DrawBackground(dl, size, dt);
    DrawTabs(dl, size);
    const float in = 1 - (1 - page_in_) * (1 - page_in_);
    page_alpha_ = fade * in;
    page_dx_ = (1 - in) * 24 * u;
    switch (page_) {
      case kHome: DrawHome(dl, size); break;
      case kContent: DrawContent(dl, size); break;
      case kSaves: DrawSaves(dl, size); break;
      case kSettings: DrawSettings(dl, size); break;
      case kTrophies: DrawTrophies(dl, size, dt); break;
      default: DrawAbout(dl, size); break;
    }
    DrawFooter(dl, size);
    if (asking_) DrawAsk(dl, size);
  }

  // Over everything: delete the files the game, the update and the DLC were installed from?
  void DrawAsk(ImDrawList* dl, const ImVec2& size) {
    const float u = u_, w = 1100 * u;
    dl->AddRectFilled(ImVec2(0, 0), size, A(IM_COL32(0, 0, 0, 170)));
    std::string list;
    for (size_t i = 0; i < ask_files_.size() && i < 5; ++i) list += Fit(g_body, 24 * u, ask_files_[i], w - 80 * u) + "\n";
    if (ask_files_.size() > 5) list += "and " + std::to_string(ask_files_.size() - 5) + " more\n";
    const std::string text = "Installed. Delete the files it was installed from (" + ask_size_ + ") from " +
                             where_ + "? They aren't needed to play. Keep them to install again later without "
                             "copying them over again.";
    const float th = g_body->CalcTextSizeA(26 * u, FLT_MAX, w - 80 * u, text.c_str()).y;
    const float lh = g_body->CalcTextSizeA(24 * u, FLT_MAX, w - 80 * u, list.c_str()).y;
    const float h = 40 * u + 56 * u + th + 20 * u + lh + 30 * u + 40 * u + 40 * u;
    const float x = (size.x - w) * 0.5f, y = (size.y - h) * 0.5f;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), A(IM_COL32(12, 14, 12, 245)), 8 * u);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 8 * u, y + h), A(kGreen));
    float ty = y + 40 * u;
    dl->AddText(g_item, 40 * u, ImVec2(x + 40 * u, ty), A(kWhite), "INSTALL COMPLETE");
    ty += 56 * u;
    dl->AddText(g_body, 26 * u, ImVec2(x + 40 * u, ty), A(IM_COL32(205, 205, 200, 255)), text.c_str(), nullptr,
                w - 80 * u);
    ty += th + 20 * u;
    dl->AddText(g_body, 24 * u, ImVec2(x + 40 * u, ty), A(kGrey), list.c_str(), nullptr, w - 80 * u);
    ty += lh + 30 * u;
    dl->AddText(g_body, 28 * u, ImVec2(x + 40 * u, ty), A(kGreen), "X  DELETE THEM          O  KEEP THEM");
  }

  ImU32 A(ImU32 c, float a = -1) const {
    if (a < 0) a = fade_;
    return (c & 0x00FFFFFFu) | (ImU32(float(c >> 24) * std::clamp(a, 0.0f, 1.0f)) << 24);
  }
  ImU32 P(ImU32 c) const { return A(c, page_alpha_); }

  // The picture slowly drifting and breathing, the city's heat flickering in it,
  // embers rising off it and the Agency's scan line over the dark.
  void DrawBackground(ImDrawList* dl, const ImVec2& size, float dt) {
    const float u = u_;
    dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(6, 8, 8, 255));
    if (background_) {
      const float cover = std::max(size.x / background_.w, size.y / background_.h);
      const float s = cover * (1.10f + 0.035f * std::sin(time_ * 0.045f));
      const float w = background_.w * s, h = background_.h * s;
      const float dx = std::sin(time_ * 0.031f) * (w - size.x) * 0.42f;
      const float dy = std::cos(time_ * 0.023f) * (h - size.y) * 0.38f;
      const ImVec2 p0((size.x - w) * 0.5f + dx, (size.y - h) * 0.5f + dy);
      // (the fire's flicker: a little brighter and warmer now and then)
      const float flicker = 0.5f + 0.25f * std::sin(time_ * 7.3f) + 0.25f * std::sin(time_ * 12.1f + 1.7f);
      const int lit = int(205 + 25 * flicker);
      dl->AddImage(background_.id, p0, ImVec2(p0.x + w, p0.y + h), ImVec2(0, 0), ImVec2(1, 1),
                   A(IM_COL32(255, std::min(255, lit + 10), lit, 255)));
      dl->AddRectFilledMultiColor(ImVec2(0, size.y * 0.55f), size, A(IM_COL32(255, 110, 30, 0)),
                                  A(IM_COL32(255, 110, 30, 0)), A(IM_COL32(255, 90, 20, int(14 + 16 * flicker))),
                                  A(IM_COL32(255, 90, 20, int(14 + 16 * flicker))));
    }
    // embers
    if (embers_.empty()) {
      embers_.resize(110);
      for (Ember& e : embers_) Respawn(e, size, true);
    }
    for (Ember& e : embers_) {
      e.life += dt;
      e.y -= e.speed * dt * u;
      e.x += (e.drift + std::sin(time_ * 1.4f + e.phase) * 14.0f) * dt * u;
      if (e.life >= e.lifetime || e.y < -20 * u) Respawn(e, size, false);
      const float k = std::sin(3.14159f * std::clamp(e.life / e.lifetime, 0.0f, 1.0f));
      const float twinkle = 0.7f + 0.3f * std::sin(time_ * 9.0f + e.phase * 3.0f);
      const int alpha = int(220 * k * twinkle);
      const ImU32 core = A(IM_COL32(255, int(170 + 60 * std::sin(e.phase)), 60, alpha));
      dl->AddCircleFilled(ImVec2(e.x, e.y), e.size * 3.2f * u, A(IM_COL32(255, 110, 30, alpha / 6)), 12);
      dl->AddCircleFilled(ImVec2(e.x, e.y), e.size * u, core, 8);
    }
    // the page's dark: the side of the menu on HOME, all of it on the others
    const float dark = page_ == kHome ? 0.0f : 0.78f;
    home_dark_ += (dark - home_dark_) * std::min(1.0f, dt * 6.0f);
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x * 0.6f, size.y), A(IM_COL32(0, 0, 0, 215)),
                                A(IM_COL32(0, 0, 0, 0)), A(IM_COL32(0, 0, 0, 0)), A(IM_COL32(0, 0, 0, 215)));
    dl->AddRectFilled(ImVec2(0, 0), size, A(IM_COL32(0, 0, 0, int(255 * home_dark_))));
    // top and bottom bars' shade
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x, 170 * u), A(IM_COL32(0, 0, 0, 200)),
                                A(IM_COL32(0, 0, 0, 200)), A(IM_COL32(0, 0, 0, 0)), A(IM_COL32(0, 0, 0, 0)));
    dl->AddRectFilledMultiColor(ImVec2(0, size.y - 150 * u), size, A(IM_COL32(0, 0, 0, 0)), A(IM_COL32(0, 0, 0, 0)),
                                A(IM_COL32(0, 0, 0, 210)), A(IM_COL32(0, 0, 0, 210)));
    // a slow scan line
    const float scan = std::fmod(time_ * 0.08f, 1.0f) * size.y;
    dl->AddRectFilled(ImVec2(0, scan), ImVec2(size.x, scan + 2 * u), A(IM_COL32(120, 220, 40, 16)));
  }

  void Respawn(Ember& e, const ImVec2& size, bool anywhere) {
    auto rnd = [this] {
      seed_ = seed_ * 1664525u + 1013904223u;
      return float(seed_ >> 8) / float(1 << 24);
    };
    e.x = rnd() * size.x;
    e.y = anywhere ? rnd() * size.y : size.y + rnd() * 40 * u_;
    e.speed = 30 + rnd() * 70;
    e.drift = (rnd() - 0.5f) * 30;
    e.size = 1.0f + rnd() * 2.2f;
    e.lifetime = 5 + rnd() * 9;
    e.life = anywhere ? rnd() * e.lifetime : 0;
    e.phase = rnd() * 6.28f;
  }

  void DrawTabs(ImDrawList* dl, const ImVec2& size) {
    const float u = u_, y = 58 * u, ts = 40 * u;
    float x = 140 * u;
    // the L1 / R1 marks either side
    auto button = [&](float bx, const char* label) {
      const float bw = Width(g_body, 20 * u, label) + 18 * u;
      dl->AddRect(ImVec2(bx, y + 9 * u), ImVec2(bx + bw, y + 37 * u), A(kGrey), 6 * u, 0, 1.5f * u);
      dl->AddText(g_body, 20 * u, ImVec2(bx + 9 * u, y + 12 * u), A(kGrey), label);
      return bw;
    };
    x += button(x, "L1") + 30 * u;
    for (int i = 0; i < kPages; ++i) {
      const float w = Width(g_item, ts, kPageNames[i]);
      if (i == page_) {
        tab_target_x_ = x, tab_target_w_ = w;
        if (tab_w_ == 0) tab_x_ = x, tab_w_ = w;
      }
      dl->AddText(g_item, ts, ImVec2(x, y), A(i == page_ ? kWhite : kGrey), kPageNames[i]);
      x += w + 52 * u;
    }
    button(x - 22 * u, "R1");
    dl->AddRectFilled(ImVec2(tab_x_, y + 56 * u), ImVec2(tab_x_ + tab_w_, y + 61 * u), A(kGreen));
    dl->AddRectFilled(ImVec2(140 * u, y + 61 * u), ImVec2(size.x - 140 * u, y + 62 * u), A(IM_COL32(255, 255, 255, 40)));
  }

  void DrawFooter(ImDrawList* dl, const ImVec2& size) {
    const float u = u_, y = size.y - 92 * u;
    dl->AddRectFilled(ImVec2(140 * u, y - 18 * u), ImVec2(size.x - 140 * u, y - 17 * u), A(IM_COL32(255, 255, 255, 40)));
    dl->AddText(g_body, 22 * u, ImVec2(140 * u, y), A(kGreen),
                Upper(Date(int64_t(std::time(nullptr)), "%a / %b %d / %I:%M %p")).c_str());
    std::string hints;
    switch (page_) {
      case kHome: hints = "X  PLAY      L1 R1  PAGES"; break;
      case kContent: hints = "UP DOWN  MOVE      X  INSTALL      TRIANGLE  UNINSTALL      O  BACK      L1 R1  PAGES"; break;
      case kSaves: hints = "UP DOWN  MOVE      X  SELECT      O  BACK      L1 R1  PAGES"; break;
      case kSettings: hints = "UP DOWN  MOVE      LEFT RIGHT  CHANGE      O  BACK      L1 R1  PAGES"; break;
      case kTrophies: hints = "UP DOWN  MOVE      LEFT RIGHT  PAGE UP / DOWN      O  BACK      L1 R1  PAGES"; break;
      default: hints = "O  BACK      L1 R1  PAGES"; break;
    }
    dl->AddText(g_body, 22 * u, ImVec2(size.x - 140 * u - Width(g_body, 22 * u, hints), y), A(kGrey), hints.c_str());
  }

  void DrawHome(ImDrawList* dl, const ImVec2& size) {
    const float u = u_, left = 140 * u + page_dx_;
    float y = 300 * u;
    if (logo_) {
      // (a wide one is the REACTIVATED wordmark: the background has the logo itself)
      const float h = (logo_.w > 3 * logo_.h ? 110 : 150) * u, w = logo_.w * h / logo_.h;
      dl->AddImage(logo_.id, ImVec2(left, y), ImVec2(left + w, y + h), ImVec2(0, 0), ImVec2(1, 1), P(kWhite));
      y += h + 40 * u;
    } else if (!background_) {
      dl->AddText(g_title, 120 * u, ImVec2(left, y), P(kWhite), "CRACKDOWN");
      y += 150 * u;
    } else {
      y += 60 * u;
    }
    // PLAY, with the Agency's pulsing bar
    const float ps = 96 * u;
    const float pulse = 0.7f + 0.3f * std::sin(time_ * 5.0f);
    dl->AddRectFilled(ImVec2(left - 38 * u, y + ps * 0.2f), ImVec2(left - 24 * u, y + ps * 0.86f),
                      P((kGreen & 0x00FFFFFFu) | (ImU32(255 * pulse) << 24)));
    const bool installing = install::TheJob().Busy();
    dl->AddText(g_item, ps, ImVec2(left, y), P(files_.Ready() && !installing ? kWhite : kDim), "PLAY");
    y += ps + 18 * u;
    if (installing) {
      y = DrawJob(dl, left, y, 900 * u) + 20 * u;
    } else if (!files_.Ready()) {
      // what's missing, and where it goes
      missing_flash_ = std::max(0.0f, missing_flash_ - 0.03f);
      const std::string what_text = Here(!files_.xex
                             ? "THE GAME ISN'T INSTALLED\nPut your Crackdown disc image (.iso) or the disc's files in "
                               "PPSA53070/install/ and open the app again: it installs by itself. The CONTENT page "
                               "shows what's there."
                             : "TITLE UPDATE 6 ISN'T INSTALLED\nPut Title Update 6's package (TU_16L61US_...1G1) in "
                               "PPSA53070/install/ and open the app again: it installs by itself.");
      const char* what = what_text.c_str();
      const float pw = 900 * u, th = g_body->CalcTextSizeA(26 * u, FLT_MAX, pw, what).y;
      dl->AddRectFilled(ImVec2(left - 16 * u, y - 12 * u), ImVec2(left + pw + 16 * u, y + th + 12 * u),
                        P(IM_COL32(40, 6, 6, ImU32(200 + 55 * missing_flash_))), 6 * u);
      dl->AddRectFilled(ImVec2(left - 16 * u, y - 12 * u), ImVec2(left - 10 * u, y + th + 12 * u),
                        P(IM_COL32(230, 50, 40, 255)));
      dl->AddText(g_body, 26 * u, ImVec2(left, y), P(kWhite), what, nullptr, pw);
      y += th + 42 * u;
    } else {
      char line[160];
      std::snprintf(line, sizeof(line), "Title Update 6%s  -  %s  -  your saves load with the game.",
                    files_.update ? "" : " (without its text files)",
                    files_.dlc == 1 ? "1 DLC pack" : (std::to_string(files_.dlc) + " DLC packs").c_str());
      dl->AddText(g_body, 26 * u, ImVec2(left, y), P(kGrey), line);
      y += 54 * u;
    }
    if (trophies_.loaded) {
      // (on a dark panel: the picture behind it is busy)
      dl->AddRectFilled(ImVec2(left - 16 * u, y - 12 * u), ImVec2(left + 560 * u, y + 48 * u),
                        P(IM_COL32(8, 8, 8, 185)), 6 * u);
      dl->AddRectFilled(ImVec2(left - 16 * u, y - 12 * u), ImVec2(left - 11 * u, y + 48 * u), P(kGreen));
      trophies::DrawCup(dl, ImVec2(left + 14 * u, y + 16 * u), 30 * u,
                        trophies_.Platinum() ? trophies::Grade::kPlatinum : trophies::Grade::kGold, page_alpha_,
                        trophies_.earned > 0);
      char line[96];
      std::snprintf(line, sizeof(line), "%d of %d trophies  -  %d%%", trophies_.earned, trophies_.earnable,
                    trophies_.Percent());
      dl->AddText(g_body, 26 * u, ImVec2(left + 40 * u, y), P(kWhite), line);
    }
  }

  // The running job's title and progress bar, or the last one's outcome for a while; the y under it.
  float DrawJob(ImDrawList* dl, float x, float y, float w) {
    const float u = u_;
    auto& job = install::TheJob();
    if (job.Busy()) {
      const float pr = job.Progress();
      char line[64];
      std::snprintf(line, sizeof(line), "%d%%", int(pr * 100));
      dl->AddRectFilled(ImVec2(x - 16 * u, y - 12 * u), ImVec2(x + w + 16 * u, y + 78 * u), P(IM_COL32(8, 8, 8, 200)),
                        6 * u);
      // (a long disc image name cut short of the percentage)
      const float pw = Width(g_body, 26 * u, line);
      dl->AddText(g_body, 26 * u, ImVec2(x, y), P(kWhite), Fit(g_body, 26 * u, Upper(job.Title()), w - pw - 30 * u).c_str());
      dl->AddText(g_body, 26 * u, ImVec2(x + w - pw, y), P(kGreen), line);
      dl->AddRectFilled(ImVec2(x, y + 46 * u), ImVec2(x + w, y + 54 * u), P(kDim));
      dl->AddRectFilled(ImVec2(x, y + 46 * u), ImVec2(x + w * pr, y + 54 * u), P(kGreen));
      return y + 90 * u;
    }
    const std::string result = job.Result();
    if (!result.empty() && time_ - result_t_ < 12.0f) {
      const float th = g_body->CalcTextSizeA(26 * u, FLT_MAX, w, result.c_str()).y;
      dl->AddRectFilled(ImVec2(x - 16 * u, y - 12 * u), ImVec2(x + w + 16 * u, y + th + 12 * u),
                        P(IM_COL32(8, 8, 8, 200)), 6 * u);
      dl->AddRectFilled(ImVec2(x - 16 * u, y - 12 * u), ImVec2(x - 10 * u, y + th + 12 * u),
                        P(job.Failed() ? IM_COL32(230, 50, 40, 255) : kGreen));
      dl->AddText(g_body, 26 * u, ImVec2(x, y), P(kWhite), result.c_str(), nullptr, w);
      return y + th + 30 * u;
    }
    return y;
  }

  static std::string Short(std::string s, size_t n) {
    if (s.size() > n) s = s.substr(0, n - 3) + "...";
    return s;
  }

  void DrawContent(ImDrawList* dl, const ImVec2& size) {
    const float u = u_, left = 140 * u + page_dx_;
    float y = 200 * u;
    const char* names[4] = {"BASE GAME", "TITLE UPDATE 6", install::DlcName(0), install::DlcName(1)};
    const char* help[4] = {
        "Crackdown itself. Put your disc image (.iso, any name) or a folder of the disc's files in "
        "PPSA53070/install/: it installs into PPSA53070/game/ when the app opens, or with X here. Afterwards the "
        "app asks whether to delete the image.",
        "Required: the game runs Title Update 6's code. Put its package (TU_16L61US_0000004000000.00000000001G1, "
        "any name) in PPSA53070/install/. Other title updates can't be used.",
        "Optional DLC (new vehicles, weapons and modes). Put its package in PPSA53070/install/.",
        "Optional DLC (new vehicles, weapons and modes). Put its package in PPSA53070/install/."};
    for (int r = 0; r < 4; ++r) {
      const bool sel = r == content_row_, in = RowInstalled(r);
      const install::Source* src = SourceFor(r);
      const float fs = 40 * u;
      if (sel) dl->AddRectFilled(ImVec2(left - 30 * u, y + 8 * u), ImVec2(left - 20 * u, y + fs), P(kGreen));
      dl->AddText(g_item, fs, ImVec2(left, y), P(sel ? kWhite : kGrey), names[r]);
      std::string status = in ? "INSTALLED" : src ? "READY TO INSTALL" : "NOT INSTALLED";
      if (sel && Confirming(100 + r)) status = "PRESS TRIANGLE AGAIN TO UNINSTALL";
      dl->AddText(g_body, 28 * u, ImVec2(left + 430 * u, y + 8 * u),
                  P(in ? kGreen : src ? kWhite : (r < 2 ? IM_COL32(230, 90, 70, 255) : kGrey)), status.c_str());
      y += fs + 6 * u;
      if (src && !in) {
        dl->AddText(g_body, 22 * u, ImVec2(left + 430 * u, y), P(kGrey), Short(src->label, 44).c_str());
        y += 30 * u;
      }
      y += 18 * u;
    }
    y += 10 * u;
    y = DrawJob(dl, left, y, 820 * u) + 10 * u;
    // the rest of install/: what can't be installed, and why
    std::vector<std::string> notes;
    for (const auto& s : sources_) {
      if (s.kind == install::Kind::kArchive)
        notes.push_back(Short(s.label, 40) + "  -  extract it on a PC first (7-Zip)");
      else if (s.kind == install::Kind::kOtherUpdate)
        notes.push_back(Short(s.label, 40) + "  -  not Title Update 6");
      else if (s.kind == install::Kind::kOther)
        notes.push_back(Short(s.label, 60));
    }
    if (!notes.empty()) {
      dl->AddText(g_body, 20 * u, ImVec2(left, y), P(kGreen), "ALSO IN THE INSTALL FOLDER");
      y += 34 * u;
      for (size_t i = 0; i < notes.size() && i < 5; ++i, y += 32 * u)
        dl->AddText(g_body, 24 * u, ImVec2(left, y), P(kGrey), notes[i].c_str());
    }
    // the focused row's help, at the right
    const float px = 1180 * u + page_dx_, pw = size.x - 140 * u - px;
    dl->AddRectFilled(ImVec2(px - 40 * u, 200 * u), ImVec2(px - 39 * u, 760 * u), P(IM_COL32(255, 255, 255, 40)));
    dl->AddText(g_body, 26 * u, ImVec2(px, 204 * u), P(IM_COL32(205, 205, 200, 255)),
                Here(help[content_row_]).c_str(), nullptr, pw);
  }

  void DrawSaves(ImDrawList* dl, const ImVec2& size) {
    const float u = u_, left = 140 * u + page_dx_;
    float y = 200 * u;
    char count[64];
    std::snprintf(count, sizeof(count), "%d %s INSTALLED", installed_.saves, installed_.saves == 1 ? "SAVE" : "SAVES");
    dl->AddText(g_body, 22 * u, ImVec2(left, y), P(kGreen), count);
    y += 48 * u;
    const auto saves = SaveSources();
    const int rows = 1 + int(saves.size());
    saves_row_ = std::min(saves_row_, rows - 1);
    for (int r = 0; r < rows; ++r) {
      const bool sel = r == saves_row_;
      const float fs = 36 * u;
      if (sel) dl->AddRectFilled(ImVec2(left - 30 * u, y + 6 * u), ImVec2(left - 20 * u, y + fs), P(kGreen));
      std::string label;
      if (r == 0) {
        label = "BACK UP SAVES";
      } else {
        const install::Source* s = saves[size_t(r - 1)];
        label = (s->kind == install::Kind::kBackup ? "RESTORE  " : "IMPORT  ") + Short(s->label, 40);
        if (sel && Confirming(200 + r)) label = "PRESS X AGAIN TO CONFIRM";
      }
      dl->AddText(g_item, fs, ImVec2(left, y), P(sel ? kWhite : kGrey), label.c_str());
      y += fs + 22 * u;
    }
    y += 10 * u;
    DrawJob(dl, left, y, 820 * u);
    const char* help =
        saves_row_ == 0
            ? "Copies your saves, your trophies and your settings to PPSA53070/install/backups/. Copy that folder "
              "to a PC or a USB drive before deleting or reinstalling the app."
        : saves[size_t(saves_row_ - 1)]->kind == install::Kind::kBackup
            ? "Puts this backup's saves, trophies and settings back, over the ones here."
            : "Imports this Xbox 360 save (a package copied off a 360, with a USB drive or Horizon) into the app. "
              "A save of the same name here is replaced.";
    const char* where =
        "\n\nTo import a save, put the Xbox 360 save package in PPSA53070/install/. Backups made here are listed "
        "to restore.";
    const std::string text = Here(std::string(help) + (saves.empty() ? where : ""));
    const float px = 1180 * u + page_dx_, pw = size.x - 140 * u - px;
    dl->AddRectFilled(ImVec2(px - 40 * u, 200 * u), ImVec2(px - 39 * u, 760 * u), P(IM_COL32(255, 255, 255, 40)));
    dl->AddText(g_body, 26 * u, ImVec2(px, 204 * u), P(IM_COL32(205, 205, 200, 255)), text.c_str(), nullptr, pw);
  }

  void DrawSettings(ImDrawList* dl, const ImVec2& size) {
    const float u = u_, left = 140 * u + page_dx_;
    float y = 200 * u;
    char music[16];
    std::snprintf(music, sizeof(music), "%d%%", settings_.music);
    struct Line {
      const char* label;
      std::string value;
      const char* help;
    } lines[kSettingsRows] = {
        {"LANGUAGE",
         settings_.language ? kLanguages[settings_.language].name
                            : std::string("AUTO (") + kLanguages[SystemLanguage()].name + ")",
         "The game's text and menus. AUTO follows the PS5's own language, or English when the game doesn't "
         "have it. Takes effect when you PLAY."},
        {"FRAME RATE", settings_.fps == 60 ? "60 FPS" : "30 FPS (LOCKED)",
         "60 runs the game as fast as the PS5 can, up to 60 frames a second. 30 locks it to the Xbox 360's "
         "own rate: lower, but steady in the busiest fights."},
        {"ANTI-ALIASING", settings_.fxaa ? "FXAA" : "OFF",
         "FXAA smooths the jagged edges of the picture as it goes to the screen. It costs the PS5's GPU a "
         "little and blurs fine detail slightly."},
        {"TEXTURE FILTERING", std::string(Settings::kFilterNames[settings_.filter]) + " ANISOTROPIC",
         "How sharp the roads, walls and rooftops stay when seen at an angle. Higher is sharper; the PS5's GPU "
         "handles 16X easily."},
        {"GPU HELPER", settings_.lane ? "ON" : "OFF",
         "A second thread that takes part of the graphics work - copying the game's memory for the GPU and "
         "writing its texture lists - off the thread that sets the frame rate. Turn it off if something "
         "draws wrong."},
        {"GPU MEMORY REFRESH", settings_.refresh ? "EVERY FRAME" : "OFF (FAST)",
         "Off, the GPU keeps the game's memory it has already copied until the game changes it. Every frame "
         "copies it all again: much slower, only for something that looks stuck or out of date."},
        {"MENU MUSIC", music, "How loud this menu's song is."},
    };
    for (int r = 0; r < kSettingsRows; ++r) {
      const bool sel = r == row_;
      const float fs = 40 * u;
      if (sel) {
        dl->AddRectFilled(ImVec2(left - 30 * u, y + 8 * u), ImVec2(left - 20 * u, y + fs), P(kGreen));
      }
      dl->AddText(g_item, fs, ImVec2(left, y), P(sel ? kWhite : kGrey), lines[r].label);
      const std::string v = r == kMusic ? lines[r].value : "<  " + lines[r].value + "  >";
      dl->AddText(g_item, fs, ImVec2(left + 420 * u, y), P(sel ? kGreen : kGrey), v.c_str());
      if (r == kMusic) {
        const float x0 = left + 540 * u, w = 260 * u, yy = y + fs * 0.6f;
        dl->AddRectFilled(ImVec2(x0, yy - 3 * u), ImVec2(x0 + w, yy + 3 * u), P(kDim));
        dl->AddRectFilled(ImVec2(x0, yy - 3 * u), ImVec2(x0 + w * settings_.music / 100.0f, yy + 3 * u),
                          P(sel ? kGreen : kGrey));
      }
      y += fs + 26 * u;
    }
    // the focused row's help, at the right
    const float px = 1060 * u + page_dx_, pw = 720 * u;
    dl->AddRectFilled(ImVec2(px - 40 * u, 200 * u), ImVec2(px - 39 * u, 760 * u), P(IM_COL32(255, 255, 255, 40)));
    dl->AddText(g_body, 28 * u, ImVec2(px, 204 * u), P(IM_COL32(205, 205, 200, 255)), lines[row_].help, nullptr, pw);
  }

  void DrawTrophies(ImDrawList* dl, const ImVec2& size, float dt) {
    const float u = u_;
    if (!trophies_.loaded) {
      dl->AddText(g_item, 44 * u, ImVec2(140 * u + page_dx_, 220 * u), P(kWhite), "START THE GAME ONCE");
      dl->AddText(g_body, 28 * u, ImVec2(140 * u + page_dx_, 290 * u), P(kGrey),
                  "The trophy list comes from the game itself: start it once, and the next time the menu opens "
                  "your trophies are here, with the ones you've earned.",
                  nullptr, 1000 * u);
      return;
    }
    const int n = int(items_.size());
    if (!n) return;
    trophy_ = std::clamp(trophy_, 0, n - 1);
    detail_in_ = std::min(1.0f, detail_in_ + dt * 6.0f);

    // the list at the left: group headings, then each trophy
    struct Row {
      int item;  // -1: a heading
      int group;
    };
    std::vector<Row> rows;
    int counts[2] = {};
    for (const Item& it : items_) ++counts[it.group];
    int sel_row = 0;
    for (int i = 0; i < n; ++i) {
      if (i == 0 || items_[size_t(i) - 1].group != items_[size_t(i)].group) rows.push_back({-1, items_[size_t(i)].group});
      if (i == trophy_) sel_row = int(rows.size());
      rows.push_back({i, items_[size_t(i)].group});
    }
    const float row_h = 64 * u, top = 172 * u, bottom = size.y - 130 * u;
    const float view = bottom - top;
    const float want = std::clamp(sel_row * row_h - view * 0.4f, 0.0f, std::max(0.0f, rows.size() * row_h - view));
    scroll_ += (want - scroll_) * std::min(1.0f, dt * 12.0f);
    const float x = 140 * u + page_dx_, right = 940 * u + page_dx_;
    dl->PushClipRect(ImVec2(0, top), ImVec2(size.x, bottom), true);
    for (size_t r = 0; r < rows.size(); ++r) {
      const float ry = top + r * row_h - scroll_;
      if (ry + row_h < top || ry > bottom) continue;
      if (rows[r].item < 0) {
        const std::string head = std::string(rows[r].group ? "NOT EARNED YET" : "EARNED") + "   " +
                                 std::to_string(counts[rows[r].group]);
        dl->AddText(g_body, 21 * u, ImVec2(x, ry + 30 * u), P(kGreen), head.c_str());
        continue;
      }
      const Item& it = items_[size_t(rows[r].item)];
      const bool sel = rows[r].item == trophy_, earned = Earned(it);
      if (sel) dl->AddRectFilled(ImVec2(x - 16 * u, ry + 4 * u), ImVec2(right + 10 * u, ry + row_h - 4 * u),
                                 P(IM_COL32(120, 220, 40, 40)), 6 * u);
      Icon(dl, it, x, ry + 8 * u, 48 * u, page_alpha_);
      const ImU32 col = sel ? kWhite : earned ? IM_COL32(210, 206, 200, 255) : kGrey;
      dl->AddText(g_item, 28 * u, ImVec2(x + 70 * u, ry + 13 * u), P(col),
                  Fit(g_item, 28 * u, NameOf(it), right - x - 130 * u).c_str());
      trophies::DrawCup(dl, ImVec2(right - 18 * u, ry + row_h * 0.5f), 30 * u, GradeOf(it), page_alpha_, earned);
    }
    dl->PopClipRect();

    // how far along, then the focused trophy
    const float px = 1040 * u + page_dx_, pw = size.x - 140 * u - px;
    float py = 180 * u;
    dl->AddRectFilled(ImVec2(px - 50 * u, top), ImVec2(px - 49 * u, bottom), P(IM_COL32(255, 255, 255, 40)));
    const std::string pct = std::to_string(trophies_.Percent()) + "%";
    dl->AddText(g_title, 72 * u, ImVec2(px, py - 10 * u), P(kWhite), pct.c_str());
    const float pctw = Width(g_title, 72 * u, pct);
    const std::string of = std::to_string(trophies_.earned) + " of " + std::to_string(trophies_.earnable) + " earned";
    dl->AddText(g_body, 26 * u, ImVec2(px + pctw + 24 * u, py + 34 * u), P(kGrey), of.c_str());
    float rx = px + pw;
    for (int g = 0; g < 3; ++g) {  // (bronze at the right, gold at the left)
      const std::string c = std::to_string(trophies_.grade_earned[g]) + "/" + std::to_string(trophies_.grade_total[g]);
      const float cw = Width(g_body, 24 * u, c);
      dl->AddText(g_body, 24 * u, ImVec2(rx - cw, py + 34 * u), P(trophies_.grade_earned[g] ? kWhite : kDim), c.c_str());
      trophies::DrawCup(dl, ImVec2(rx - cw - 20 * u, py + 48 * u), 26 * u, trophies::Grade(g), page_alpha_,
                        trophies_.grade_earned[g] > 0);
      rx -= cw + 60 * u;
    }
    py += 92 * u;
    dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + 8 * u), P(IM_COL32(255, 255, 255, 30)));
    const float f = trophies_.points ? float(trophies_.points_earned) / float(trophies_.points) : 0.0f;
    if (f > 0) dl->AddRectFilled(ImVec2(px, py), ImVec2(px + std::max(4 * u, pw * f), py + 8 * u),
                                 P(trophies_.Platinum() ? trophies::GradeColor(trophies::Grade::kPlatinum) : kGreen));
    py += 54 * u;

    const Item& it = items_[size_t(trophy_)];
    const float da = page_alpha_ * detail_in_;
    const float dx = px + (1 - detail_in_) * 14 * u;
    Icon(dl, it, dx, py, 112 * u, da);
    const trophies::Grade g = GradeOf(it);
    const std::string grade = Upper(std::string(trophies::GradeName(g)) + " trophy");
    dl->AddText(g_body, 20 * u, ImVec2(dx + 136 * u, py + 8 * u), trophies::GradeColor(g, da), grade.c_str());
    dl->AddText(g_item, 36 * u, ImVec2(dx + 136 * u, py + 34 * u), A(kWhite, da), NameOf(it).c_str(), nullptr,
                pw - 136 * u);
    py += 138 * u;
    const std::string text = TextOf(it);
    dl->AddText(g_body, 27 * u, ImVec2(dx, py), A(IM_COL32(205, 205, 200, 255), da), text.c_str(), nullptr, pw);
    py += g_body->CalcTextSizeA(27 * u, FLT_MAX, pw, text.c_str()).y + 34 * u;
    const trophies::Trophy* t = it.index < 0 ? nullptr : &trophies_.trophies[size_t(it.index)];
    std::string status;
    if (Earned(it)) status = "Earned " + Date(t ? t->earned : trophies_.last_earned, "%b %d, %Y");
    else if (!t) status = std::to_string(trophies_.earnable - trophies_.earned) + " to go";
    else status = "Not earned yet";
    dl->AddText(g_body, 20 * u, ImVec2(dx, py), A(kDim, da), "STATUS");
    dl->AddText(g_body, 24 * u, ImVec2(dx + 170 * u, py - 2 * u), A(kWhite, da), status.c_str());
    if (t) {
      py += 40 * u;
      dl->AddText(g_body, 20 * u, ImVec2(dx, py), A(kDim, da), "GAMERSCORE");
      dl->AddText(g_body, 24 * u, ImVec2(dx + 170 * u, py - 2 * u), A(kWhite, da),
                  (std::to_string(t->gamerscore) + "G").c_str());
    }
  }

  void DrawAbout(ImDrawList* dl, const ImVec2& size) {
    const float u = u_, x = 140 * u + page_dx_, w = 820 * u;
    float y = 190 * u;
    auto entry = [&](const char* name, const char* text) {
      dl->AddText(g_item, 32 * u, ImVec2(x, y), P(kWhite), name);
      y += 44 * u;
      dl->AddText(g_body, 24 * u, ImVec2(x, y), P(kGrey), text, nullptr, w);
      y += g_body->CalcTextSizeA(24 * u, FLT_MAX, w, text).y + 30 * u;
    };
    dl->AddText(g_body, 20 * u, ImVec2(x, y), P(kGreen), "CRACKDOWN: REACTIVATED");
    y += 40 * u;
    entry("BY CHAOSTHEORY1075", "github.com/ChaosTheory1075/crackdown-ps5");
    dl->AddText(g_body, 20 * u, ImVec2(x, y), P(kGreen), "ALL THE CREDIT GOES TO");
    y += 40 * u;
    entry("GTA IV FOR PS5  -  ELRIPALDA",
          "The PS5 port this one is built on, top to bottom: the PS5 runtime, graphics and sound, the hub this menu "
          "copies, the trophies and their pop-up, and the installer payload. github.com/elripalda/gta4-ps5");
    entry("CRACKDOWN RECOMPILED  -  SAULOB",
          "The PC recompilation of Crackdown: the game fixes, the 60 FPS frame pacing and the configuration this "
          "port runs. github.com/saulob/crackdown-xbox360-recompiled");
    entry("LIBERTY RECOMPILED  -  OZORDI  /  VADUUR",
          "OZORDI's recompilation of GTA IV and vaduur's LibertyRecompAndroid, which GTA IV for PS5 starts from.");
    entry("REXGLUE AND XENIA",
          "The static recompiler and runtime under all of it, and the Xenia team's Xbox 360 emulator it grew from.");
    const float px = 1060 * u + page_dx_, pw = size.x - 140 * u - px;
    float py = 190 * u;
    // the shout-out, on a panel of its own
    {
      const char* thanks =
          "For making Crackdown: a unique, amazing game that will always hold up. Pacific City is still the best "
          "playground there is.";
      const float th = g_body->CalcTextSizeA(26 * u, FLT_MAX, pw - 48 * u, thanks).y;
      dl->AddRectFilled(ImVec2(px - 24 * u, py - 16 * u), ImVec2(px + pw, py + 96 * u + th),
                        P(IM_COL32(8, 8, 8, 170)), 6 * u);
      dl->AddRectFilled(ImVec2(px - 24 * u, py - 16 * u), ImVec2(px - 18 * u, py + 96 * u + th), P(kGreen));
      dl->AddText(g_body, 20 * u, ImVec2(px, py), P(kGreen), "SPECIAL THANKS");
      dl->AddText(g_item, 40 * u, ImVec2(px, py + 30 * u), P(kWhite), "REALTIME WORLDS");
      dl->AddText(g_body, 26 * u, ImVec2(px, py + 84 * u), P(kGrey), thanks, nullptr, pw - 48 * u);
      py += 96 * u + th + 50 * u;
    }
    dl->AddText(g_body, 20 * u, ImVec2(px, py), P(kGreen), "PLEASE NOTE");
    py += 36 * u;
    const char* note =
        "An unofficial fan project, not affiliated with or endorsed by Microsoft, Realtime Worlds or Sony. "
        "Crackdown and all related names belong to their owners. No game files are included: you need your own "
        "copy of Crackdown for the Xbox 360.";
    dl->AddText(g_body, 24 * u, ImVec2(px, py), P(kGrey), note, nullptr, pw);
    py += g_body->CalcTextSizeA(24 * u, FLT_MAX, pw, note).y + 40 * u;
    dl->AddText(g_body, 20 * u, ImVec2(px, py), P(kGreen), "BUILT WITH");
    py += 36 * u;
    dl->AddText(g_body, 22 * u, ImVec2(px, py), P(kGrey),
                "PS5 Payload SDK\nMesa RADV (Vulkan)\nDear ImGui\nstb_image, dr_mp3\nOswald and Inter (SIL Open Font License)\n"
                "BlackBearReloaded's PS5 AT9 Converter (the home screen music)");
  }

  // ---- files ----

  void StartMusic(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      if (!e.is_regular_file() || !gta4ps5::sound::CanOpen(e.path())) continue;
      if (!gta4ps5::sound::StartOutput()) {
        REXLOG_WARN("Crackdown menu: no audio port for the menu's song");
        return;
      }
      if (auto src = gta4ps5::sound::Open(e.path())) {
        music_ = gta4ps5::sound::NewVoice();
        music_->SetVolume(settings_.music / 100.0f);
        music_->Play(std::move(src), true);
        REXLOG_INFO("Crackdown menu: playing {}", e.path().filename().string());
        return;
      }
    }
  }

  Image Load(const fs::path& file) {
    if (!immediate_) return {};
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(file.string().c_str(), &w, &h, &n, 4);
    if (!px) return {};
    auto tex = immediate_->CreateTexture(uint32_t(w), uint32_t(h), rex::ui::ImmediateTextureFilter::kLinear, false, px);
    stbi_image_free(px);
    if (!tex) return {};
    Image img{reinterpret_cast<ImTextureID>(tex.get()), float(w), float(h)};
    g_textures->push_back(std::move(tex));
    return img;
  }

  rex::ui::ImmediateDrawer* immediate_ = nullptr;
  fs::path app_, user_data_;
  std::string where_;  // (the install folder's real place, for the screen)
  GameFiles files_;
  float missing_flash_ = 0;
  install::Installed installed_;
  std::vector<install::Source> sources_;
  std::set<std::string> tried_;  // (installed by itself once each)
  // the question after the installs: delete what they were installed from?
  bool asking_ = false;
  std::vector<std::string> ask_files_;
  std::string ask_size_;
  uint32_t job_generation_ = 0;
  float result_t_ = -100;
  int content_row_ = 0, saves_row_ = 0, confirm_key_ = -1;
  float confirm_t_ = 0;
  std::function<void()> play_;
  Settings settings_;
  Pad pad_;
  Image background_, logo_;
  gta4ps5::sound::Voice* music_ = nullptr;
  Page page_ = kHome;
  int row_ = kFrameRate, trophy_ = 0;
  trophies::List trophies_;
  std::vector<Item> items_;
  std::map<uint32_t, Image> pictures_;
  int pictures_this_frame_ = 0;
  std::vector<Ember> embers_;
  uint32_t seed_ = 0x4D5307DCu;
  float time_ = 0, leave_t_ = 0, fade_ = 0, u_ = 1, page_in_ = 1, page_alpha_ = 1, page_dx_ = 0;
  float tab_x_ = 0, tab_w_ = 0, tab_target_x_ = 0, tab_target_w_ = 0, home_dark_ = 0, scroll_ = 0, detail_in_ = 1;
  bool leaving_ = false, done_ = false;
};

ImFont* AddFont(ImFontAtlas* atlas, const unsigned char* begin, const unsigned char* end, float size) {
  ImFontConfig cfg;
  cfg.FontDataOwnedByAtlas = false;  // (built into the app)
  cfg.OversampleH = 2;
  return atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(begin), int(end - begin), size, &cfg);
}

}  // namespace

void ConfigureMenuFonts(ImFontAtlas* atlas) {
  // (at the 4K screen's sizes: drawn smaller on a 1080p one)
  g_title = AddFont(atlas, cdps5_font_title, cdps5_font_title_end, 240.0f);
  g_item = AddFont(atlas, cdps5_font_item, cdps5_font_item_end, 128.0f);
  g_body = AddFont(atlas, cdps5_font_body, cdps5_font_body_end, 56.0f);
}

ImFont* MenuBodyFont() { return g_body; }
ImFont* MenuItemFont() { return g_item; }

void ShowMenu(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, const fs::path& app,
              std::function<void()> play) {
  REXLOG_INFO("Crackdown menu: shown");
  new MenuDialog(drawer, immediate, app, std::move(play));  // (the dialog owns itself)
}

}  // namespace cdps5

#endif  // REX_PLATFORM_PS5
