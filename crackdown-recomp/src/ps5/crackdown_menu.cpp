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
#include <rex/input/sony_feedback.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>

#include "cd3_setup.h"
#include "cd_install.h"
#include "cd_ptbr_install.h"
#include "cd_text.h"
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
int sceSystemServiceLoadExec(const char* path, char* const argv[]);
// Starts another installed app (libSceSystemService). Used to start Prospero Win straight into Crackdown 3.
struct CdLncAppParam {
  uint32_t sz;
  int32_t user_id;
  uint32_t app_opt;
  uint64_t crash_report;
  uint32_t check_flag;
};
int sceLncUtilInitialize(void);
int sceLncUtilLaunchApp(const char* title_id, const char* argv[], CdLncAppParam* param);
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
  kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kCross = 16, kCircle = 32, kL1 = 64, kR1 = 128, kTriangle = 256,
  kSquare = 512
};

constexpr uint32_t kPadUp = 0x00000010u, kPadRight = 0x00000020u, kPadDown = 0x00000040u,
                   kPadLeft = 0x00000080u, kPadL1 = 0x00000400u, kPadR1 = 0x00000800u,
                   kPadTriangle = 0x00001000u, kPadCircle = 0x00002000u, kPadCross = 0x00004000u,
                   kPadSquare = 0x00008000u;
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
    if (d.buttons & kPadSquare) b |= kSquare;
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
  const char* pt;  // (its name in the menu's Portuguese)
};
// (PORTUGUESE (BR), Brazilian Portuguese, is the title's text folder 15, pt-BR: in its folder table, not on the disc. The
// menu is in Portuguese with it; the game's text once a pt-BR folder is installed, English until then.)
constexpr int kPortugueseLocale = 15;
constexpr Language kLanguages[] = {
    {"AUTO", -1, 0, "AUTO"},                  {"ENGLISH", 0, 1, "INGLÊS"},
    {"FRENCH", 1, 4, "FRANCÊS"},              {"GERMAN", 3, 3, "ALEMÃO"},
    {"ITALIAN", 2, 6, "ITALIANO"},            {"SPANISH", 4, 5, "ESPANHOL"},
    {"SPANISH (LATAM)", 5, 5, "ESPANHOL (AL)"}, {"PORTUGUESE (BR)", kPortugueseLocale, 1, "PORTUGUÊS (BR)"},
    {"POLISH", 7, 1, "POLONÊS"},              {"CZECH", 6, 1, "TCHECO"},
    {"HUNGARIAN", 8, 1, "HÚNGARO"},           {"SLOVAK", 13, 1, "ESLOVACO"},
    {"JAPANESE", 9, 2, "JAPONÊS"},            {"KOREAN", 10, 7, "COREANO"},
    {"CHINESE", 11, 8, "CHINÊS"},
    {"RUSSIAN", 14, 1, "RUSSO"}};  // (Crackdown 2's disc only: ru-RU)
constexpr int kRussianLocale = 14;
constexpr int kLanguageCount = int(sizeof(kLanguages) / sizeof(kLanguages[0]));

// A text folder's name, by its number (Crackdown 2's LANGUAGE: crackdown2_locale, cd2_features.cpp).
const char* LocaleFolder(int locale) {
  static constexpr const char* kFolders[] = {"en-US", "fr-FR", "it-IT", "de-DE", "es-ES", "es-LA", "cs-CZ", "pl-PL",
                                             "hu-HU", "ja-JP", "ko-KR", "zh-TW", "",      "sk-SK", "ru-RU", "pt-BR"};
  return locale >= 0 && locale < 16 ? kFolders[locale] : "";
}

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
    case 8: return LanguageIndex(kRussianLocale);  // Russian (Crackdown 2's; English in Crackdown 1)
    case 23: return LanguageIndex(6);            // Czech
    case 24: return LanguageIndex(8);            // Hungarian
    case 7: case 17: return LanguageIndex(kPortugueseLocale);  // Portuguese, Portuguese (Brazil)
    default: return LanguageIndex(0);
  }
}

// Whether a Portuguese (Brazil) text folder is installed: game/Local/pt-BR (any case), the title's
// text folder 15 (locale_ps5.cpp).
bool PortugueseTextInstalled() {
  std::error_code ec;
  for (const auto& local : fs::directory_iterator("/app0/game", ec)) {
    if (Upper(local.path().filename().string()) != "LOCAL" || !local.is_directory(ec)) continue;
    std::error_code ec2;
    for (const auto& e : fs::directory_iterator(local.path(), ec2))
      if (Upper(e.path().filename().string()) == "PT-BR" && e.is_directory(ec2)) return true;
  }
  return false;
}

struct Settings {
  int language = 0;      // kLanguages: AUTO, or the text folder the game is given (crackdown_locale)
  int fps = 60;        // crackdown_frame_rate: 60 (the title's variable step) or 30 (locked)
  int fps2 = 60;       // crackdown2_frame_rate (Crackdown 2): 60 (its measured clock) unless the user picks 30
  bool fps2_chosen = false;  // cd2_frame_rate_chosen: the user picked fps2 on Crackdown 2's FRAME RATE row
  int aa = 2;            // swap_post_effect: 0 none, 1 fxaa, 2 smaa (Saints Row: ReInstated's)
  bool fsr = true;       // the picture's stretch to the TV: FSR 1 (present_effect fsr) or bilinear
  int sharpness = 3;     // FSR 1's sharpening (present_fsr_sharpness_reduction): 1 low, 2 medium, 3 high
  bool host_rts = true;  // RENDERER: FAST (host render targets, what Crackdown always drew with) or ACCURATE (fsi)
  bool cheat_menu = true;      // L3 + R3 opens the in-game quick menu (crackdown_quick_menu, cd_quick_menu.cpp)
  int dualsense_strength = 3;  // 1..3: low, medium, high (gta4_sony_intensity); HIGH as the user plays (2.0.0)
  int filter = 5;        // anisotropic_override 2..5: 2x, 4x, 8x, 16x; 16X as the user plays (2.0.0)
  bool lane = true;      // the GPU's second lane (crackdown_gpu_lane)
  bool refresh = false;  // clear_memory_page_state: every page re-read from the game each frame
  int music = 60;        // the menu song's level, 0..100
  bool ps_buttons = true;   // crackdown_ps_buttons: PlayStation button prompts (cd_glyphs.h)
  bool dualsense = true;    // the light bar, adaptive triggers and added rumble (dualsense_ps5.cpp)
  int counter = 0;          // crackdown_fps_counter: 0 off, 1 simple, 2 detailed (cd_overlay.h)
  bool skip_intro = true;   // crackdown_skip_intro: straight to the title screen
  int game = 1;             // the game HOME has chosen: 1 Crackdown, 2 Crackdown 2

  static constexpr const char* kFilterNames[] = {"", "", "2X", "4X", "8X", "16X"};
  static constexpr const char* kCounterModes[] = {"off", "simple", "detailed"};
  static constexpr const char* kAaNames[] = {"OFF", "FXAA", "SMAA"};
  static constexpr const char* kSharpnessNames[] = {"", "LOW", "MEDIUM", "HIGH"};
  // FSR 1's RCAS sharpness reduction in stops (0 = sharpest) for each sharpness.
  static constexpr const char* kSharpnessStops[] = {"", "1.0", "0.5", "0.2"};

  // The recommended settings (SQUARE on SETTINGS): the user's own for both games (menu1.ini / menu2.ini on the
  // console, 2026-11-13, the 2.0.0 defaults too). The menu's song level stays as it is.
  static Settings Recommended(const Settings& now) {
    Settings s = now;
    s.language = LanguageIndex(-1), s.fps = 60, s.fps2 = 60, s.aa = 2, s.fsr = true, s.sharpness = 3, s.host_rts = true;
    s.cheat_menu = true, s.dualsense_strength = 3, s.filter = 5, s.lane = true, s.refresh = false;
    s.ps_buttons = true, s.dualsense = true, s.counter = 0, s.skip_intro = true;
    return s;
  }

  // menu.ini (the last started game's: the game reads it when it starts, and its quick menu writes it),
  // or a game's own, menu1.ini / menu2.ini (Saints Row: ReInstated's).
  static Settings Load(const fs::path& app, const char* file = "menu.ini") {
    Settings s;
    if (std::FILE* f = std::fopen((app / file).string().c_str(), "rb")) {
      char line[256];
      while (std::fgets(line, sizeof(line), f)) {
        char key[64] = {};
        int value = 0;
        // (keys with digits too: cd2_frame_rate was never read with [a-z_], so Crackdown 2 always got the default)
        if (std::sscanf(line, " %63[a-z0-9_] = %d", key, &value) != 2) continue;
        const std::string k(key);
        if (k == "game_language") s.language = LanguageIndex(value);
        if (k == "frame_rate") s.fps = value == 30 ? 30 : 60;
        if (k == "cd2_frame_rate") s.fps2 = value == 30 ? 30 : 60;
        if (k == "cd2_frame_rate_chosen") s.fps2_chosen = value != 0;
        if (k == "fxaa" && value) s.aa = 1;  // (the earlier on/off row's)
        if (k == "anti_aliasing") s.aa = std::clamp(value, 0, 2);
        if (k == "upscaling_fsr") s.fsr = value != 0;
        if (k == "fsr_sharpness") s.sharpness = std::clamp(value, 1, 3);
        if (k == "renderer_fast") s.host_rts = value != 0;
        if (k == "cheat_menu") s.cheat_menu = value != 0;
        if (k == "dualsense_strength") s.dualsense_strength = std::clamp(value, 1, 3);
        if (k == "texture_filtering") s.filter = std::clamp(value, 2, 5);
        if (k == "menu_music") s.music = std::clamp(value, 0, 100);
        if (k == "gpu_lane") s.lane = value != 0;
        if (k == "memory_refresh") s.refresh = value != 0;
        if (k == "ps_buttons") s.ps_buttons = value != 0;
        if (k == "dualsense") s.dualsense = value != 0;
        if (k == "fps_counter") s.counter = std::clamp(value, 0, 2);
        if (k == "skip_intro") s.skip_intro = value != 0;
        if (k == "game") s.game = value == 2 ? 2 : 1;
      }
      std::fclose(f);
    }
    // (earlier builds wrote their default, 30, into every file: only a 30 the user picked stays 30)
    if (!s.fps2_chosen) s.fps2 = 60;
    return s;
  }

  void Save(const fs::path& app, const char* file = "menu.ini") const {
    if (std::FILE* f = std::fopen((app / file).string().c_str(), "wb")) {
      std::fprintf(f, "game = %d\ngame_language = %d\nframe_rate = %d\nanti_aliasing = %d\nupscaling_fsr = %d\n"
                   "fsr_sharpness = %d\ntexture_filtering = %d\ngpu_lane = %d\nmemory_refresh = %d\n"
                   "renderer_fast = %d\nmenu_music = %d\nps_buttons = %d\ndualsense = %d\n"
                   "dualsense_strength = %d\nfps_counter = %d\nskip_intro = %d\ncheat_menu = %d\ncd2_frame_rate = %d\n"
                   "cd2_frame_rate_chosen = %d\n",
                   game, kLanguages[language].locale, fps, aa, fsr ? 1 : 0, sharpness, filter, lane ? 1 : 0,
                   refresh ? 1 : 0, host_rts ? 1 : 0, music, ps_buttons ? 1 : 0, dualsense ? 1 : 0,
                   dualsense_strength, counter, skip_intro ? 1 : 0, cheat_menu ? 1 : 0, fps2, fps2_chosen ? 1 : 0);
      std::fclose(f);
    }
  }

  // Whether the menu's language is Portuguese (Brazil): the chosen one, or the PS5's for AUTO.
  bool IsPortuguese() const { return kLanguages[language ? language : SystemLanguage()].locale == kPortugueseLocale; }

  // The cvars they set, for this start.
  void Apply() const {
    // (the Xbox 360's own resolution, always: 2x cost the PS5 far more than it showed)
    rex::cvar::SetFlagByName("draw_resolution_scale_x", "1");
    rex::cvar::SetFlagByName("draw_resolution_scale_y", "1");
    rex::cvar::SetFlagByName("crackdown_frame_rate", std::to_string(fps));
#if defined(CD_GAME) && CD_GAME == 2
    rex::cvar::SetFlagByName("crackdown2_frame_rate", std::to_string(fps2));  // (cd2_features.cpp)
#endif
    rex::cvar::SetFlagByName("swap_post_effect", aa == 2 ? "smaa" : aa == 1 ? "fxaa" : "none");
    rex::cvar::SetFlagByName("present_effect", fsr ? "fsr" : "bilinear");
    rex::cvar::SetFlagByName("present_fsr_sharpness_reduction", kSharpnessStops[sharpness]);
    {
      // (init-only, and the start's flags are final by now; the GPU is set up after this menu)
      rex::cvar::testing::ScopedLifecycleOverride override_init_only;  // (always built, cvar.cpp)
      rex::cvar::SetFlagByName("render_target_path_vulkan", host_rts ? "fbo" : "fsi");
    }
    rex::cvar::SetFlagByName("crackdown_quick_menu", cheat_menu ? "true" : "false");
    rex::cvar::SetFlagByName("gta4_sony_intensity",
                             dualsense_strength == 1 ? "0.3" : dualsense_strength == 3 ? "1.0" : "0.6");
    rex::cvar::SetFlagByName("anisotropic_override", std::to_string(filter));
    rex::cvar::SetFlagByName("crackdown_gpu_lane", lane ? "true" : "false");
    // (off: the pages the GPU read stay valid until the game writes them - as Xenia's own default; on,
    // every one of them was copied again each frame, 93% of them unchanged in crackdown_003.log)
    rex::cvar::SetFlagByName("clear_memory_page_state", refresh ? "true" : "false");
    const Language& l = kLanguages[language ? language : SystemLanguage()];
    // (Portuguese's text folder only once it's installed: the game's English text until then)
    const int locale = l.locale == kPortugueseLocale && !PortugueseTextInstalled() ? 0
                       : l.locale == kRussianLocale ? 0  // (Crackdown 1 has no Russian: English)
                                                    : l.locale;
    // Crackdown 2: its folder by name (cd2.bin reads crackdown2_locale; Crackdown 1's program hasn't it)
    rex::cvar::SetFlagByName("crackdown2_locale", game == 2 ? LocaleFolder(l.locale) : "");
    SetPortuguese(l.locale == kPortugueseLocale);  // (the quick menu's and the trophy pop-up's text)
    rex::cvar::SetFlagByName("crackdown_locale", std::to_string(locale));
    rex::cvar::SetFlagByName("user_language", std::to_string(l.xbox));
    rex::cvar::SetFlagByName("crackdown_ps_buttons", ps_buttons ? "true" : "false");
    rex::input::sony::SetFeaturesEnabled(dualsense);
    rex::cvar::SetFlagByName("crackdown_fps_counter", kCounterModes[counter]);
    rex::cvar::SetFlagByName("crackdown_skip_intro", skip_intro ? "true" : "false");
    REXLOG_INFO("Crackdown menu: {} button prompts, DualSense features {} ({} feedback), FPS counter {}, skip intro {}, "
                "cheat menu {}",
                ps_buttons ? "PlayStation" : "Xbox", dualsense ? "on" : "off", kSharpnessNames[dualsense_strength],
                kCounterModes[counter], skip_intro ? "on" : "off", cheat_menu ? "on" : "off");
    REXLOG_INFO("Crackdown menu: language {}{} (text folder {}, Xbox 360 language {}), {} fps, upscaling {} (sharpness {}), "
                "anti-aliasing {}, texture filtering {}, the GPU's second lane {}, memory refresh {}, renderer {}", l.name,
                language ? "" : " (AUTO)", l.locale, l.xbox, fps, fsr ? "FSR 1" : "bilinear", kSharpnessNames[sharpness],
                kAaNames[aa], kFilterNames[filter], lane ? "on" : "off", refresh ? "every frame" : "off",
                host_rts ? "fast (host render targets)" : "accurate (fsi)");
  }
};

// ---- the look ----

constexpr ImU32 kWhite = IM_COL32(236, 232, 226, 255);
constexpr ImU32 kGrey = IM_COL32(150, 146, 140, 255);
constexpr ImU32 kDim = IM_COL32(90, 88, 84, 255);
constexpr ImU32 kBlue = IM_COL32(70, 165, 255, 255);  // (the accent: the logo's neon blue)
constexpr ImU32 kText = IM_COL32(205, 205, 200, 255);

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

// strftime's, with %a and %b in Portuguese while the menu is (the PS5's libc only has the C locale's English).
std::string Date(int64_t t, const char* format) {
  const std::time_t tt = std::time_t(t);
  const std::tm* tm = std::localtime(&tt);
  if (!tm) return {};
  std::string f = format;
  if (Portuguese()) {
    static constexpr const char* kDays[] = {"dom", "seg", "ter", "qua", "qui", "sex", "sáb"};
    static constexpr const char* kMonths[] = {"jan", "fev", "mar", "abr", "mai", "jun",
                                              "jul", "ago", "set", "out", "nov", "dez"};
    for (size_t at; (at = f.find("%a")) != std::string::npos;) f.replace(at, 2, kDays[tm->tm_wday % 7]);
    for (size_t at; (at = f.find("%b")) != std::string::npos;) f.replace(at, 2, kMonths[tm->tm_mon % 12]);
  }
  char out[64] = {};
  std::strftime(out, sizeof(out), f.c_str(), tm);
  return out;
}

// Letter-spaced text, a whole UTF-8 letter at a time; returns its width.
float Spaced(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 col, const char* t, float spacing) {
  const float x0 = p.x;
  for (const char* c = t; *c;) {
    const char* next = c + 1;
    while ((static_cast<unsigned char>(*next) & 0xC0) == 0x80) ++next;
    dl->AddText(f, size, p, col, c, next);
    p.x += f->CalcTextSizeA(size, FLT_MAX, 0, c, next).x + spacing;
    c = next;
  }
  return p.x - x0 - spacing;
}

// The footer's button hints, with SQUARE and TRIANGLE (QUADRADO, TRIÂNGULO) drawn as the DualSense's
// symbols, outlined like its buttons (X and O read as theirs already); returns the width (draw false:
// measured only).
float Hints(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 col, const std::string& t, bool draw) {
  static constexpr const char* kWords[] = {"SQUARE", "QUADRADO", "TRIANGLE", "TRIÂNGULO"};
  const float x0 = p.x, icon = size * 0.66f, line = std::max(1.0f, size * 0.075f);
  size_t i = 0;
  while (i < t.size()) {
    size_t at = std::string::npos;
    int word = -1;
    for (int k = 0; k < 4; ++k) {
      const size_t a = t.find(kWords[k], i);
      if (a < at) at = a, word = k;
    }
    const size_t end = at == std::string::npos ? t.size() : at;
    if (end > i) {
      if (draw) dl->AddText(f, size, p, col, t.c_str() + i, t.c_str() + end);
      p.x += f->CalcTextSizeA(size, FLT_MAX, 0, t.c_str() + i, t.c_str() + end).x;
    }
    if (at == std::string::npos) break;
    const float cy = p.y + size * 0.52f, h = icon * 0.5f;
    if (draw && word < 2)
      dl->AddRect(ImVec2(p.x + line, cy - h + line), ImVec2(p.x + icon - line, cy + h - line), col, 0, 0, line * 1.6f);
    else if (draw)
      dl->AddTriangle(ImVec2(p.x + icon * 0.5f, cy - h), ImVec2(p.x + icon, cy + h * 0.8f), ImVec2(p.x, cy + h * 0.8f),
                      col, line * 1.6f);
    p.x += icon;
    i = at + std::char_traits<char>::length(kWords[word]);
  }
  return p.x - x0;
}

// The two games, as HOME lists them ([1], [2]).
constexpr const char* kGameTitles[4] = {"", "CRACKDOWN", "CRACKDOWN 2", "CRACKDOWN 3"};
// HOME's third entry: Crackdown 3, the player's own PC (UWP) copy run by Prospero Win (PPSA99995) through its
// "cd3" profile. Only the settings and trophies pages are Crackdown and Crackdown 2's (games 1 and 2).
constexpr int kCd3 = 3;
constexpr const char* kProsperoWinTitle = "PPSA99995";
// What Crackdown 3 has on the console, and its install and sign-in jobs: cd3_setup.h.

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
enum Page { kHome, kContent, kSaves, kSettings, kTrophies, kCredits, kPages };
const char* PageName(int page) {
  static constexpr const char* kEn[kPages] = {"HOME", "CONTENT", "SAVES", "SETTINGS", "TROPHIES", "CREDITS"};
  static constexpr const char* kPt[kPages] = {"INÍCIO", "CONTEÚDO", "SAVES", "AJUSTES", "TROFÉUS", "CRÉDITOS"};
  return (Portuguese() ? kPt : kEn)[page];
}

// An ember rising from the city.
struct Ember {
  float x = 0, y = 0, speed = 0, drift = 0, size = 0, life = 0, lifetime = 1, phase = 0;
};

class MenuDialog final : public rex::ui::ImGuiDialog {
 public:
  MenuDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, const fs::path& app,
             std::function<void()> play)
      : ImGuiDialog(drawer), immediate_(immediate), app_(app), play_(std::move(play)) {
    SwapInNewCd2();
    LoadSettings();
    const fs::path hub = app / "hub";
    // Crackdown's: hub/background.jpg (or background1), Crackdown 2's: hub/background2.jpg
    for (const char* name : {"background.jpg", "background.png", "background1.jpg", "background1.png"})
      if (!background_[1]) background_[1] = Load(hub / name);
    for (const char* name : {"background2.jpg", "background2.png"})
      if (!background_[2]) background_[2] = Load(hub / name);
    for (const char* name : {"background3.jpg", "background3.png"})
      if (!background_[kCd3]) background_[kCd3] = Load(hub / name);
    logo_ = Load(hub / "logo.png");
    title_ = Load(hub / "title.png");
    game_ = set_game_ = trophy_game_ = sets_[1].game;
    shown_game_ = float(game_);
    StartMusic(game_);
    // (an install the app was closed during: not to be taken for a finished one)
    {
      std::error_code ec;
      for (const char* name : {"game.installing", "update.installing", "game2.installing", "update2.installing"})
        fs::remove_all(app / name, ec);
      for (const char* dlc : {"dlc", "dlc2"}) {
        for (fs::directory_iterator it(app / dlc, ec), end; !ec && it != end; it.increment(ec))
          if (it->path().extension() == ".installing") fs::remove_all(it->path(), ec);
        ec.clear();
      }
    }
#if !defined(CD_GAME) || CD_GAME != 2
    InstallPortugueseText(app);  // (game/Local/pt-BR when it's missing: cd_ptbr_install.h)
#endif
    Refresh();
    where_ = install::Where(app);
    REXLOG_INFO("Crackdown menu: game {}, Title Update 6 {}, its other files {}, {} DLC packs",
                files_.xex ? "found" : "MISSING", files_.xexp ? "found" : "MISSING",
                files_.update ? "found" : "missing", files_.dlc);
    REXLOG_INFO("Crackdown menu: Crackdown 2 {}, Title Update 5 {}, {} DLC packs, {} in install/",
                installed_.game2 ? "found" : "MISSING", installed_.update2 ? "found" : "MISSING", installed_.dlc2,
                sources_.size());
    // (the runtime's user data: /app0/UserData/crackdown and crackdown2)
    for (int g = 1; g <= 2; ++g) {
      user_data_[g] = app / "UserData" / (g == 2 ? "crackdown2" : "crackdown");
      lists_[g] = trophies::Load(user_data_[g]);
      REXLOG_INFO("Crackdown menu: {} trophies {} ({} of {} earned)", kGameTitles[g],
                  lists_[g].loaded ? "listed" : "not listed yet", lists_[g].earned, lists_[g].earnable);
    }
    // Crackdown 3's: its list comes from Xbox Live at its first start after the sign-in; unlocks as it plays.
    user_data_[3] = app / "cd3";
    lists_[3] = trophies::LoadCd3(user_data_[3]);
    REXLOG_INFO("Crackdown menu: CRACKDOWN 3 trophies {} ({} of {} earned)", lists_[3].loaded ? "listed" : "not listed yet",
                lists_[3].earned, lists_[3].earnable);
    ShowTrophies(game_);
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (done_) return;
    const float dt = std::min(io.DeltaTime, 0.1f);
    time_ += dt;
    pictures_this_frame_ = 0;
    // (the menu's text follows LANGUAGE as it changes: the game's whose settings are shown)
    SetPortuguese(sets_[page_ == kSettings ? set_game_ : std::min(game_, 2)].IsPortuguese());
    HandlePad(pad_.Pressed());
    TickInstaller();
    if (cd3_job_.kind != cd3::Job::Kind::kNone && (cd3_tick_ += dt) > 0.4f) {
      cd3_tick_ = 0;
      const bool was = cd3_job_.Active();
      cd3::Tick(app_, cd3_job_, time_);
      if (was && !cd3_job_.Active()) Refresh();  // (installed, or signed in)
    }
    if (music_) music_->SetVolume(leaving_ ? 0.0f : sets_[1].music / 100.0f);
    tab_x_ += (tab_target_x_ - tab_x_) * std::min(1.0f, dt * 14.0f);
    tab_w_ += (tab_target_w_ - tab_w_) * std::min(1.0f, dt * 14.0f);
    page_in_ = std::min(1.0f, page_in_ + dt * 5.0f);
    Draw(io.DisplaySize, dt);
    if (leaving_ && (leave_t_ += dt) > 0.7f) {
      SaveSettings();
      if (game_ == kCd3) {
        // Prospero Win reads "profile=<id>" from its arguments and starts that game, skipping its own menu
        // (prospero-win src/pw_wine_launch.c). This app may not start another one itself (it faulted inside
        // sceLncUtilInitialize, crackdown_067): the payload, which runs with system rights, starts it.
        REXLOG_INFO("Crackdown menu: starting Crackdown 3 through the payload (Prospero Win {} profile=cd3)",
                    kProsperoWinTitle);
        rex::FlushLogging();
        std::string error;
        // BUTTON PROMPTS for Crackdown 3: the hub's choice (Crackdown 1's settings, the set the menu itself
        // follows). The payload puts its PlayStation pak in or takes it out (ps5/payload/installer/cd3_prompts.inc).
        const std::string extra = std::string("prompts=") + (sets_[1].ps_buttons ? "ps" : "xbox") + "\n";
        if (!cd3::Start(app_, cd3::Job::Kind::kLaunch, cd3_job_, error, extra)) start_error_ = error;
        leaving_ = false;
        leave_t_ = 0;
        StartMusic(game_);
        return;
      }
      if (game_ == 2) {
        // Crackdown 2 is its own program (cd2.bin): started in this one's place, it applies menu.ini itself
        // (as Saints Row: ReInstated's sr2.bin)
        SwapInNewCd2();
        REXLOG_INFO("Crackdown menu: starting Crackdown 2 (cd2.bin)");
        rex::FlushLogging();
        char* args[] = {nullptr};
        const int rc = sceSystemServiceLoadExec("/app0/cd2.bin", args);
        REXLOG_ERROR("Crackdown menu: starting cd2.bin failed ({:#x})", uint32_t(rc));
        start_error_ = T("Couldn't start Crackdown 2 (cd2.bin is missing from the app folder, or the PS5 refused it).",
                         "Não deu para iniciar o Crackdown 2 (falta o cd2.bin na pasta do app, ou o PS5 o recusou).");
        leaving_ = false;
        leave_t_ = 0;
        StartMusic(game_);
        return;
      }
      done_ = true;
      sets_[game_].Apply();
      Close();
      play_();
    }
  }

 private:
  // The settings page's rows, in its order (a new option: a row here, its line in SettingsLines and its
  // change in ChangeSetting).
  enum SettingsRow {
    kFrameRate, kUpscaling, kSharpness, kAntiAliasing, kFiltering, kLane, kCounter, kButtons, kDualSense,
    kDualSenseStrength, kLanguageRow, kSkipIntro, kCheatMenu, kRenderer, kRefresh, kMusic, kSettingsRows
  };

  // ---- the installer (cd_install.h) ----

  // `s` with the install folder's real place (any drive, any name) for "PPSA53070/install/".
  std::string Here(std::string s) const {
    for (const std::pair<std::string, std::string>& swap :
         {std::pair<std::string, std::string>{"PPSA53070/install/", where_ + "/"},
          std::pair<std::string, std::string>{"PPSA53070/game/", T("the app's game folder", "a pasta game do app")}})
      for (size_t at; (at = s.find(swap.first)) != std::string::npos;) s.replace(at, swap.first.size(), swap.second);
    return s;
  }

  void Refresh() {
    files_ = GameFiles::Find(app_);
    installed_ = install::Check(app_);
    cd3_ = cd3::Setup::Find(app_);
    sources_ = install::Scan(app_);
    // (an imported save's file deleted: its row gone, the selection kept on the list)
    saves_row_ = std::min(saves_row_, int(SaveSources().size()));
  }

  // The content rows, in install::Remove's order: Crackdown's game, TU6 and two DLC packs, then Crackdown 2's
  // game, Title Update 5 and its DLC (any number of packs, one row).
  enum ContentRow { kRowGame, kRowUpdate, kRowDlc0, kRowDlc1, kRowGame2, kRowUpdate2, kRowDlc2, kContentRows };

  // Whether one of Crackdown 2's DLC packages is installed (dlc2/<its name>).
  bool Dlc2Installed(const install::Source& s) const {
    std::error_code ec;
    const fs::path d = app_ / "dlc2" / s.path.filename();
    return fs::is_directory(d, ec) && !fs::is_empty(d, ec);
  }
  int Dlc2Waiting() const {
    int n = 0;
    for (const auto& s : sources_) n += s.kind == install::Kind::kDlc && s.game == 2 && !Dlc2Installed(s);
    return n;
  }
  // The content row a source installs into.
  static int RowOf(const install::Source& s) {
    const bool game = s.kind == install::Kind::kGameImage || s.kind == install::Kind::kGameFolder;
    const bool update = s.kind == install::Kind::kUpdate || s.kind == install::Kind::kOtherUpdate;
    if (s.game == 2) return game ? kRowGame2 : update ? kRowUpdate2 : kRowDlc2;
    return game ? kRowGame : update ? kRowUpdate : s.kind == install::Kind::kDlc ? kRowDlc0 + s.dlc : -1;
  }

  // What waits in install/ for a content row; nullptr: nothing.
  const install::Source* SourceFor(int row) const {
    for (const auto& s : sources_) {
      const bool installable = s.kind == install::Kind::kGameImage || s.kind == install::Kind::kGameFolder ||
                               s.kind == install::Kind::kUpdate || s.kind == install::Kind::kOtherUpdate ||
                               s.kind == install::Kind::kDlc;
      if (!installable || RowOf(s) != row) continue;
      if (row == kRowDlc2 && Dlc2Installed(s)) continue;
      return &s;
    }
    return nullptr;
  }
  bool RowInstalled(int row) const {
    switch (row) {
      case kRowGame: return installed_.game;
      case kRowUpdate: return installed_.update;
      case kRowDlc0: return installed_.dlc[0];
      case kRowDlc1: return installed_.dlc[1];
      case kRowGame2: return installed_.game2;
      case kRowUpdate2: return installed_.update2;
      default: return installed_.dlc2 > 0;
    }
  }
  // The chosen game's disc and title update: what PLAY needs.
  bool Ready() const {
    if (game_ == kCd3) return cd3_.Playable() && cd3_.signed_in && !cd3_job_.Active();
    return game_ == 2 ? installed_.game2 && installed_.update2 : files_.Ready();
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
#if !defined(CD_GAME) || CD_GAME != 2
      InstallPortugueseText(app_);  // (a game or Title Update 6 just installed: its pt-BR folder now)
#endif
      Refresh();
    }
    // (not in the first seconds: the menu on screen first, the progress with it)
    if (job.Busy() || leaving_ || time_ < 2.0f) return;
    for (const auto& s : sources_) {
      if (!install::Automatic(s.kind) || tried_.count(s.path.string())) continue;
      const int row = RowOf(s);
      if (row < 0 || (row == kRowDlc2 ? Dlc2Installed(s) : RowInstalled(row))) continue;
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
      if (p & kCircle) {
        REXLOG_INFO("Crackdown install: kept the {} files it was installed from (O)",
                    install::TheJob().Leftovers().size());
        install::TheJob().ForgetLeftovers();
      }
      if (p & (kCross | kCircle)) asking_ = false;
      return;
    }
    if (p & (kL1 | kR1)) {
      page_ = Page((page_ + ((p & kR1) ? 1 : kPages - 1)) % kPages);
      page_in_ = 0;
      // (SETTINGS opens on the game list, at the game HOME has chosen)
      if (page_ == kSettings) set_game_ = std::min(game_, 2), in_list_ = false, confirm_key_ = -1;
      return;
    }
    if ((p & kCircle) && page_ == kSettings && in_list_) {
      in_list_ = false;  // (back to the game first)
      return;
    }
    if ((p & kCircle) && page_ != kHome) {
      page_ = kHome;
      page_in_ = 0;
      return;
    }
    switch (page_) {
      case kHome:
        if (p & (kUp | kDown)) {
          game_ = (p & kDown) ? game_ % 3 + 1 : (game_ + 1) % 3 + 1;
          start_error_.clear();
          if (game_ == kCd3) Refresh();  // (set up since the menu opened?)
          StartMusic(game_);
          if (game_ <= 2) ShowTrophies(game_);
        }
        if (game_ == kCd3 && (p & kCircle) && cd3_job_.Active()) {
          cd3::Stop(app_);  // (the sign-in or install stops)
          break;
        }
        if (game_ == kCd3 && (p & kCross) && !Ready()) {
          Cd3Cross();
          break;
        }
        if ((p & kCross) && install::TheJob().Busy()) break;  // (PLAY waits for the install)
        if ((p & kCross) && !Ready()) {
          Refresh();  // (copied in since the menu opened?)
          missing_flash_ = 1.0f;
        }
        if ((p & kCross) && Ready()) {
          leaving_ = true;
          if (music_) music_->FadeOut(0.6f);
        }
        break;
      case kContent: {
        if (p & kUp) content_row_ = (content_row_ + kContentRows - 1) % kContentRows;
        if (p & kDown) content_row_ = (content_row_ + 1) % kContentRows;
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
        // SQUARE, twice: the game's recommended settings back
        if ((p & kSquare) && Confirmed(300 + set_game_)) {
          sets_[set_game_] = Settings::Recommended(sets_[set_game_]);
          reset_t_ = time_;
          REXLOG_INFO("Crackdown menu: {}'s recommended settings put back", kGameTitles[set_game_]);
        }
        if (!in_list_) {
          // the game list: whose settings
          if (p & (kUp | kDown)) set_game_ = set_game_ == 1 ? 2 : 1, confirm_key_ = -1;
          if (p & (kRight | kCross)) in_list_ = true;  // (into its settings)
          break;
        }
        if (p & kUp) row_ = (row_ + kSettingsRows - 1) % kSettingsRows;
        if (p & kDown) row_ = (row_ + 1) % kSettingsRows;
        const int step = (p & kRight) ? 1 : (p & kLeft) ? -1 : 0;
        ChangeSetting(row_, step, (p & kCross) != 0);
        break;
      }
      case kTrophies: {
        if (p & kTriangle) {  // (the other game's)
          ShowTrophies(trophy_game_ % 3 + 1);
          break;
        }
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

  // ---- the settings ----

  // Each game's own: menu1.ini / menu2.ini, and menu.ini, the last started game's (Crackdown 2's program
  // reads it: ApplySettings). Before there were per-game files, menu.ini was Crackdown's.
  // Crackdown 2's set is always menu2.ini's (or, before there was one, menu.ini's when Crackdown 2 was the
  // last started): never Crackdown 1's, whose FRAME RATE is its own. From menu.ini it only takes what
  // Crackdown 2's quick menu wrote there (FPS COUNTER, DUALSENSE) when it was the last started.
  // A cd2.bin uploaded while Crackdown 2 was running (the console won't replace a running program) waits as
  // cd2.bin.new: swapped in here, where cd2.bin isn't running, so an upload never waits for the game to close.
  void SwapInNewCd2() {
    std::error_code ec;
    if (!fs::exists(app_ / "cd2.bin.new", ec)) return;
    fs::rename(app_ / "cd2.bin.new", app_ / "cd2.bin", ec);
    if (ec) REXLOG_ERROR("Crackdown menu: cd2.bin.new not swapped in ({})", ec.message());
    else REXLOG_INFO("Crackdown menu: the new cd2.bin swapped in");
  }

  void LoadSettings() {
    const Settings last = Settings::Load(app_);
    std::error_code ec;
    const bool own1 = fs::exists(app_ / "menu1.ini", ec), own2 = fs::exists(app_ / "menu2.ini", ec);
    sets_[1] = own1 ? Settings::Load(app_, "menu1.ini") : last;
    if (last.game == 1) sets_[1] = last;
    if (own2) {
      sets_[2] = Settings::Load(app_, "menu2.ini");
      if (last.game == 2) sets_[2].counter = last.counter, sets_[2].dualsense = last.dualsense;
    } else if (last.game == 2) {
      sets_[2] = last;
    } else {
      sets_[2] = last;
      sets_[2].fps2 = Settings().fps2, sets_[2].fps2_chosen = false;  // (Crackdown 1's file: not its pick)
    }
    for (int g = 1; g <= 2; ++g) sets_[g].game = last.game, sets_[g].music = last.music;
    REXLOG_INFO("Crackdown menu: Crackdown 2's frame rate {} ({}, from {})", sets_[2].fps2,
                sets_[2].fps2_chosen ? "the user's pick" : "the default", own2 ? "menu2.ini" : "menu.ini");
  }

  // At PLAY: each game's file, and menu.ini as the game starting reads it.
  void SaveSettings() {
    for (int g = 1; g <= 2; ++g) {
      sets_[g].game = game_;
      sets_[g].Save(app_, ("menu" + std::to_string(g) + ".ini").c_str());
    }
    sets_[std::min(game_, 2)].Save(app_);
  }

  // The settings SETTINGS shows (the game picked at its left).
  Settings& Shown() { return sets_[set_game_]; }
  const Settings& Shown() const { return sets_[set_game_]; }
  bool Cd2Shown() const { return set_game_ == 2; }  // (the settings shown are Crackdown 2's)
  int& ShownFps() { return Cd2Shown() ? Shown().fps2 : Shown().fps; }  // (each game's FRAME RATE)
  int ShownFps() const { return Cd2Shown() ? Shown().fps2 : Shown().fps; }

  struct SettingLine {
    const char* section;  // the heading it's under
    const char* label;
    std::string value;
    bool enabled;
    std::string help;
    bool slider = false;  // (a level: the bar, not < >)
  };

  std::vector<SettingLine> SettingsLines() const {
    char music[16];
    std::snprintf(music, sizeof(music), "%d%%", Shown().music);
    const char* on = T("ON", "LIGADO");
    const char* off = T("OFF", "DESLIGADO");
    const char* graphics = T("GRAPHICS", "GRÁFICOS");
    const char* controller = T("CONTROLLER", "CONTROLE");
    const char* game = T("GAME", "JOGO");
    static constexpr const char* kLevelsPt[] = {"", "BAIXA", "MÉDIA", "ALTA"};
    std::vector<SettingLine> lines(kSettingsRows);
    lines[kFrameRate] = {graphics, T("FRAME RATE", "TAXA DE QUADROS"),
                         ShownFps() == 60 ? "60 FPS" : T("30 FPS (LOCKED)", "30 FPS (TRAVADO)"), true,
                         T("60 runs the game as fast as the PS5 can, up to 60 frames a second. 30 locks it to the Xbox "
                           "360's own rate: lower, but steady in the busiest fights.",
                           "60 roda o jogo o mais rápido que o PS5 consegue, até 60 quadros por segundo. 30 trava na "
                           "taxa do próprio Xbox 360: menor, mas estável nas lutas mais pesadas.")};
    lines[kUpscaling] = {graphics, T("UPSCALING", "AMPLIAÇÃO"), Shown().fsr ? "FSR 1" : "BILINEAR", true,
                         T("How the picture is enlarged to your TV's resolution (up to 4K). FSR 1, AMD's upscaler, keeps "
                           "edges and detail sharp at almost no cost to the frame rate; BILINEAR is a plain, softer "
                           "stretch.",
                           "Como a imagem é ampliada para a resolução da sua TV (até 4K). O FSR 1, o ampliador da AMD, "
                           "mantém bordas e detalhes nítidos quase sem custo para a taxa de quadros; BILINEAR é uma "
                           "ampliação simples, mais suave.")};
    lines[kSharpness] = {graphics, T("SHARPNESS", "NITIDEZ"),
                         Shown().fsr ? T(Settings::kSharpnessNames[Shown().sharpness], kLevelsPt[Shown().sharpness])
                                       : "-",
                         Shown().fsr,
                         T("How much FSR 1 sharpens the enlarged picture. HIGH is crispest but can make edges look "
                           "harsh; LOW is gentler.",
                           "Quanto o FSR 1 deixa a imagem ampliada mais nítida. ALTA é a mais definida, mas pode deixar "
                           "as bordas duras; BAIXA é mais suave.")};
    lines[kAntiAliasing] = {graphics, T("ANTI-ALIASING", "ANTISSERRILHAMENTO"),
                            Shown().aa ? Settings::kAaNames[Shown().aa] : off, true,
                            T("Smooths jagged edges before the picture is enlarged. SMAA finds edges by their shape and "
                              "keeps the rest sharp; FXAA is a little cheaper but blurs fine detail. Both cost the "
                              "PS5's GPU very little.",
                              "Suaviza bordas serrilhadas antes de a imagem ser ampliada. O SMAA encontra as bordas pela "
                              "forma e mantém o resto nítido; o FXAA é um pouco mais leve, mas borra detalhes finos. "
                              "Os dois custam muito pouco à GPU do PS5.")};
    lines[kFiltering] = {graphics, T("TEXTURE FILTERING", "FILTRO DE TEXTURAS"),
                         std::string(Settings::kFilterNames[Shown().filter]) + T(" ANISOTROPIC", " ANISOTRÓPICO"),
                         true,
                         T("How sharp the roads, walls and rooftops stay when seen at an angle. Higher is sharper; the "
                           "PS5's GPU handles 16X easily.",
                           "O quanto ruas, paredes e telhados continuam nítidos vistos de lado. Mais alto é mais "
                           "nítido; a GPU do PS5 aguenta 16X com facilidade.")};
    lines[kLane] = {graphics, T("GPU HELPER", "AUXILIAR DA GPU"), Shown().lane ? on : off, true,
                    T("A second thread that takes part of the graphics work - copying the game's memory for the GPU "
                      "and writing its texture lists - off the thread that sets the frame rate. Turn it off if "
                      "something draws wrong.",
                      "Uma segunda thread que tira parte do trabalho gráfico - copiar a memória do jogo para a GPU e "
                      "escrever as listas de texturas - da thread que define a taxa de quadros. Desligue se algo for "
                      "desenhado errado.")};
    static constexpr const char* kCounterNames[] = {"OFF", "SIMPLE", "DETAILED"};
    static constexpr const char* kCounterNamesPt[] = {"DESLIGADO", "SIMPLES", "DETALHADO"};
    lines[kCounter] = {graphics, T("FPS COUNTER", "CONTADOR DE FPS"),
                       T(kCounterNames[Shown().counter], kCounterNamesPt[Shown().counter]), true,
                       T("The frame rate while you play, at the top left of the screen. Simple shows frames a second "
                         "and the frame time; detailed adds the slowest frames and a graph of the last two seconds.",
                         "A taxa de quadros durante o jogo, no canto superior esquerdo da tela. Simples mostra os "
                         "quadros por segundo e o tempo de quadro; detalhado também os quadros mais lentos e um "
                         "gráfico dos últimos dois segundos.")};
    lines[kButtons] = {controller, T("BUTTON PROMPTS", "ÍCONES DOS BOTÕES"),
                       Shown().ps_buttons ? "PLAYSTATION" : "XBOX 360", true,
                       T("The buttons the game shows in its prompts and menus: the DualSense's, or the Xbox 360's it "
                         "was made with. The first start in a language builds its font, which takes a few seconds.",
                         "Os botões que o jogo mostra nas dicas e menus: os do DualSense, ou os do Xbox 360 para o qual "
                         "foi feito. A primeira vez em cada idioma monta a fonte dele, o que leva alguns segundos.")};
    lines[kDualSense] = {controller, T("DUALSENSE FEATURES", "RECURSOS DO DUALSENSE"), Shown().dualsense ? on : off,
                         true,
                         T("The light bar in the Agency's green, red when you're hit; adaptive triggers that push back "
                           "like pedals when you drive and like a trigger when you shoot; a kick with each shot and "
                           "hit. The game's own rumble stays either way.",
                           "A barra de luz no verde da Agência, vermelha quando você é atingido; gatilhos adaptáveis que "
                           "resistem como pedais quando você dirige e como um gatilho quando você atira; um tranco a "
                           "cada tiro e golpe. A vibração do próprio jogo continua de qualquer jeito.")};
    static constexpr const char* kStrengths[] = {"", "LOW", "MEDIUM", "HIGH"};
    lines[kDualSenseStrength] = {controller, T("FEEDBACK STRENGTH", "FORÇA DO FEEDBACK"),
                                 Shown().dualsense ? T(kStrengths[Shown().dualsense_strength],
                                                         kLevelsPt[Shown().dualsense_strength])
                                                     : "-",
                                 Shown().dualsense,
                                 T("How strong the DualSense's added rumble, kicks and trigger resistance are.",
                                   "A força da vibração extra, dos trancos e da resistência dos gatilhos do DualSense.")};
    auto language_name = [](int i) { return T(kLanguages[i].name, kLanguages[i].pt); };
    lines[kLanguageRow] = {game, T("LANGUAGE", "IDIOMA"),
                           Shown().language ? std::string(language_name(Shown().language))
                                              : std::string("AUTO (") + language_name(SystemLanguage()) + ")",
                           true,
                           T("The game's text, menus and subtitles: any of the 13 languages on the disc, or "
                             "PORTUGUESE (BR), Brazilian Portuguese: this menu in it, and the game's text too once a "
                             "pt-BR text folder is installed (English until then). AUTO follows the PS5's own "
                             "language, or English when the game doesn't have it.",
                             "Os textos, menus e legendas do jogo: qualquer um dos 13 idiomas do disco, ou PORTUGUÊS "
                             "(BR), o português do Brasil: este menu nele, e o texto do jogo também assim que uma "
                             "pasta de textos pt-BR estiver instalada (em inglês até lá). AUTO segue o idioma do PS5, "
                             "ou inglês quando o jogo não o tem.")};
    lines[kSkipIntro] = {game, T("SKIP INTRO", "PULAR INTRODUÇÃO"), Shown().skip_intro ? on : off, true,
                         T("Straight to the title screen, past the logos and the opening movie.",
                           "Direto para a tela de título, sem os logotipos e o vídeo de abertura.")};
    lines[kCheatMenu] = {game, T("CHEAT MENU (L3 + R3)", "TRAPAÇAS (L3 + R3)"), Shown().cheat_menu ? on : off, true,
                         T("In the game, press both sticks in (L3 + R3) for the quick menu: the cheats, the orbs on the "
                           "map, the FPS counter and the DualSense features. OFF: no quick menu, and no cheats.",
                           "No jogo, aperte os dois analógicos (L3 + R3) para o menu rápido: as trapaças, os orbes no "
                           "mapa, o contador de FPS e os recursos do DualSense. DESLIGADO: sem menu rápido e sem "
                           "trapaças.")};
    lines[kRenderer] = {"EXPERIMENTAL", T("RENDERER", "RENDERIZADOR"),
                        Shown().host_rts ? T("FAST", "RÁPIDO") : T("ACCURATE", "PRECISO"), true,
                        T("FAST draws with the PS5's own graphics hardware, as Crackdown: ReActivated always has. "
                          "ACCURATE copies the Xbox 360's video memory exactly, pixel by pixel: slower, for "
                          "something that draws wrong only. Testing.",
                          "RÁPIDO desenha com o próprio hardware gráfico do PS5, como o Crackdown: ReActivated sempre "
                          "fez. PRECISO copia a memória de vídeo do Xbox 360 exatamente, pixel por pixel: mais lento, "
                          "só para algo que é desenhado errado. Em teste.")};
    lines[kRefresh] = {"EXPERIMENTAL", T("GPU MEMORY REFRESH", "MEMÓRIA DA GPU"),
                       Shown().refresh ? T("EVERY FRAME", "TODO QUADRO") : T("OFF (FAST)", "DESLIGADO (RÁPIDO)"), true,
                       T("Off, the GPU keeps the game's memory it has already copied until the game changes it. Every "
                         "frame copies it all again: much slower, only for something that looks stuck or out of date.",
                         "Desligado, a GPU mantém a memória do jogo que já copiou até o jogo mudá-la. Todo quadro "
                         "copia tudo de novo: bem mais lento, só para algo que parece travado ou desatualizado.")};
    lines[kMusic] = {T("AUDIO", "ÁUDIO"), T("MENU MUSIC", "MÚSICA DO MENU"), music, true,
                     T("How loud this menu's song is.", "O volume da música deste menu."), true};
    if (Cd2Shown()) {
      // Crackdown 2: only what its program does (cd2_features.cpp); the rest dimmed, saying why.
      auto locked = [&](int row, const char* value, const char* en, const char* pt) {
        lines[size_t(row)].enabled = false;
        lines[size_t(row)].value = value;
        lines[size_t(row)].help = T(en, pt);
      };
      // (FRAME RATE: Crackdown 2 has its 60 too now, cd2_features.cpp crackdown_frame_rate)
      lines[kButtons].help = T("The game's buttons as the DualSense's: Cross, Circle, Square, Triangle, L1/R1, L2/R2, the "
                               "sticks, the D-pad, OPTIONS and the touch pad, in the HUD hints and the menus.",
                               "Os botões do jogo como os do DualSense: X, Círculo, Quadrado, Triângulo, L1/R1, L2/R2, os "
                               "analógicos, o direcional, OPTIONS e o touch pad, nas dicas da tela e nos menus.");
      locked(kDualSenseStrength, "-",
             "The added kicks and trigger resistance need Crackdown 2's player found in the game first: not in this "
             "version yet. The game's own rumble plays.",
             "Os trancos e a resistência dos gatilhos precisam que o jogador do Crackdown 2 seja encontrado no jogo "
             "primeiro: ainda não nesta versão. A vibração do próprio jogo funciona.");
      lines[kCheatMenu].help =
          T("In the game, press both sticks in (L3 + R3) for the quick menu: Infinite Health and Infinite Ammo (the "
            "game's own god mode and ammo switches), the FPS counter and the DualSense features. O closes it.",
            "No jogo, aperte os dois analógicos (L3 + R3) para o menu rápido: Vida Infinita e Munição Infinita (os "
            "próprios modos do jogo), o contador de FPS e os recursos do DualSense. O fecha.");
      lines[kDualSense].help = T("The light bar in Crackdown 2's blue. The game's own rumble stays either way.",
                                 "A barra de luz no azul do Crackdown 2. A vibração do próprio jogo continua de "
                                 "qualquer jeito.");
      lines[kSkipIntro].help = T("Straight to the title screen, past the MGS and Ruffian logos and the autosave notice.",
                                 "Direto para a tela de título, sem os logotipos da MGS e da Ruffian e o aviso de "
                                 "salvamento automático.");
      lines[kLanguageRow].help =
          T("The game's text, menus and subtitles: any of the 14 languages on the disc (Czech, Hungarian, Slovak, "
            "Korean and Chinese have text only: their speech is English, as on the Xbox 360), or PORTUGUESE (BR) once a "
            "pt-BR text folder is installed (English until then). AUTO follows the PS5's own language.",
            "Os textos, menus e legendas do jogo: qualquer um dos 14 idiomas do disco (tcheco, húngaro, eslovaco, "
            "coreano e chinês têm só texto: a fala é em inglês, como no Xbox 360), ou PORTUGUÊS (BR) assim que uma "
            "pasta de textos pt-BR estiver instalada (em inglês até lá). AUTO segue o idioma do PS5.");
    }
    return lines;
  }

  void ChangeSetting(int row, int step, bool cross) {
    if (!SettingsLines()[size_t(row)].enabled) return;  // (a dimmed row: nothing to change)
    const bool flip = step || cross;
    switch (row) {
      case kFrameRate:
        if (flip) ShownFps() = ShownFps() == 60 ? 30 : 60;
        if (flip && Cd2Shown()) {
          // (the user's pick: kept, and saved now, so no later start or app close can lose it)
          Shown().fps2_chosen = true;
          Shown().Save(app_, "menu2.ini");
          REXLOG_INFO("Crackdown menu: Crackdown 2's frame rate set to {} by the user", Shown().fps2);
        }
        break;
      case kUpscaling:
        if (flip) Shown().fsr = !Shown().fsr;
        break;
      case kSharpness:
        if (!Shown().fsr) break;
        if (cross) Shown().sharpness = Shown().sharpness % 3 + 1;
        Shown().sharpness = std::clamp(Shown().sharpness + step, 1, 3);
        break;
      case kAntiAliasing:
        if (cross) Shown().aa = (Shown().aa + 1) % 3;
        Shown().aa = std::clamp(Shown().aa + step, 0, 2);
        break;
      case kFiltering:
        if (cross) Shown().filter = Shown().filter == 5 ? 2 : Shown().filter + 1;
        Shown().filter = std::clamp(Shown().filter + step, 2, 5);
        break;
      case kLane:
        if (flip) Shown().lane = !Shown().lane;
        break;
      case kCounter:
        if (cross) Shown().counter = (Shown().counter + 1) % 3;
        Shown().counter = std::clamp(Shown().counter + step, 0, 2);
        break;
      case kButtons:
        if (flip) Shown().ps_buttons = !Shown().ps_buttons;
        break;
      case kDualSense:
        if (flip) Shown().dualsense = !Shown().dualsense;
        break;
      case kDualSenseStrength:
        if (!Shown().dualsense) break;
        if (cross) Shown().dualsense_strength = Shown().dualsense_strength % 3 + 1;
        Shown().dualsense_strength = std::clamp(Shown().dualsense_strength + step, 1, 3);
        break;
      case kLanguageRow: {
        const int d = step ? step : cross ? 1 : 0;
        int v = Shown().language;
        do v = (v + d + kLanguageCount) % kLanguageCount;  // (Russian: Crackdown 2's only)
        while (d && !Cd2Shown() && kLanguages[v].locale == kRussianLocale);
        Shown().language = v;
        break;
      }
      case kSkipIntro:
        if (flip) Shown().skip_intro = !Shown().skip_intro;
        break;
      case kCheatMenu:
        if (flip) Shown().cheat_menu = !Shown().cheat_menu;
        break;
      case kRenderer:
        if (flip) Shown().host_rts = !Shown().host_rts;
        break;
      case kRefresh:
        if (flip) Shown().refresh = !Shown().refresh;
        break;
      case kMusic:
        Shown().music = std::clamp(Shown().music + step * 10, 0, 100);
        sets_[1].music = sets_[2].music = Shown().music;  // (the menu's: the same for both games)
        break;
    }
  }

  // ---- the trophies ----

  // The list's groups.
  enum Group { kEarned, kNotYet, kOnline };

  // A row of the list: a trophy (its index; -1: the platinum), in its group.
  struct Item {
    int index = 0, group = 0;
  };

  // The trophies page shows one game's list at a time (TRIANGLE: the other's).
  void ShowTrophies(int game) {
    trophy_game_ = game;
    trophy_ = 0;
    scroll_ = 0;
    pictures_.clear();
    TakeTrophies(lists_[game]);
  }

  void TakeTrophies(trophies::List list) {
    trophies_ = std::move(list);
    items_.clear();
    if (!trophies_.loaded) return;
    const auto& all = trophies_.trophies;
    std::vector<Item> earned, rest, online;
    for (int i = 0; i < int(all.size()); ++i) {
      const auto& t = all[size_t(i)];
      if (t.online()) online.push_back({i, kOnline});
      else (t.earned ? earned : rest).push_back({i, t.earned ? kEarned : kNotYet});
    }
    std::stable_sort(earned.begin(), earned.end(), [&](const Item& a, const Item& b) {
      return all[size_t(a.index)].earned > all[size_t(b.index)].earned;
    });
    const bool plat = trophies_.Platinum();
    if (plat) items_.push_back({-1, kEarned});
    items_.insert(items_.end(), earned.begin(), earned.end());
    if (!plat) items_.push_back({-1, kNotYet});
    items_.insert(items_.end(), rest.begin(), rest.end());
    items_.insert(items_.end(), online.begin(), online.end());
  }

  bool Earned(const Item& it) const {
    return it.index < 0 ? trophies_.Platinum() : trophies_.trophies[size_t(it.index)].earned != 0;
  }
  trophies::Grade GradeOf(const Item& it) const {
    return it.index < 0 ? trophies::Grade::kPlatinum : trophies_.trophies[size_t(it.index)].grade();
  }
  std::string NameOf(const Item& it) const {
    if (it.index < 0)
      return trophy_game_ == 3   ? T("Crackdown 3 Platinum", "Platina de Crackdown 3")
             : trophy_game_ == 2 ? T("Crackdown 2 Platinum", "Platina de Crackdown 2")
                                 : T("Crackdown Platinum", "Platina de Crackdown");
    const auto& t = trophies_.trophies[size_t(it.index)];
    return (!t.earned && t.hidden()) ? T("Hidden trophy", "Troféu oculto") : t.name;
  }
  std::string TextOf(const Item& it) const {
    if (it.index < 0)
      return (trophy_game_ == 3   ? T("Earn every other Crackdown 3 trophy (", "Conquiste todos os outros troféus de Crackdown 3 (")
              : trophy_game_ == 2 ? T("Earn every other Crackdown 2 trophy (", "Conquiste todos os outros troféus de Crackdown 2 (")
                                : T("Earn every other Crackdown trophy (", "Conquiste todos os outros troféus de Crackdown (")) +
             std::to_string(trophies_.earnable) + ").";
    const auto& t = trophies_.trophies[size_t(it.index)];
    std::string text;
    if (!t.earned && t.hidden()) text = T("Keep playing to find out what this one is.", "Continue jogando para descobrir qual é este.");
    else text = t.earned || t.locked_description.empty() ? t.description : t.locked_description;
    if (t.online())
      text += T("\n\nOnline only: it needs Co-op, a squad or Xbox LIVE, which this port doesn't have, so it doesn't "
                "count toward the platinum.",
                "\n\nSó online: precisa do Co-op, de um esquadrão ou da Xbox LIVE, que este port não tem, então não "
                "conta para a platina.");
    return text;
  }

  // A trophy's picture (the platinum's: the game's own), loaded the first time it's drawn
  // (two a frame at most, so the page doesn't hold the menu up).
  Image Picture(const Item& it) {
    const uint32_t key = it.index < 0 ? 0 : std::max(1u, trophies_.trophies[size_t(it.index)].image_id);
    if (auto found = pictures_.find(key); found != pictures_.end()) return found->second;
    if (pictures_this_frame_ >= 2) return {};
    ++pictures_this_frame_;
    const fs::path& user = user_data_[trophy_game_];
    const Image img =
        trophy_game_ == 3
            ? (it.index < 0 ? Image{} : Load(trophies::Cd3IconFile(user, trophies_.trophies[size_t(it.index)].image_id)))
            : Load(it.index < 0 ? trophies::TitleIconFile(user)
                                : trophies::IconFile(user, trophies_.trophies[size_t(it.index)].image_id));
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
      default: DrawCredits(dl, size); break;
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
    if (ask_files_.size() > 5) list += T("and ", "e mais ") + std::to_string(ask_files_.size() - 5) + T(" more\n", "\n");
    const std::string text =
        T("Installed. Delete the files it was installed from (", "Instalado. Apagar os arquivos usados na instalação (") +
        ask_size_ + T(") from ", ") de ") + where_ +
        T("? They aren't needed to play. Keep them to install again later without copying them over again.",
          "? Eles não são necessários para jogar. Guarde-os para instalar de novo depois sem copiá-los outra vez.");
    const float th = g_body->CalcTextSizeA(26 * u, FLT_MAX, w - 80 * u, text.c_str()).y;
    const float lh = g_body->CalcTextSizeA(24 * u, FLT_MAX, w - 80 * u, list.c_str()).y;
    const float h = 40 * u + 56 * u + th + 20 * u + lh + 30 * u + 40 * u + 40 * u;
    const float x = (size.x - w) * 0.5f, y = (size.y - h) * 0.5f;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), A(IM_COL32(12, 14, 12, 245)), 8 * u);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 8 * u, y + h), A(kBlue));
    float ty = y + 40 * u;
    dl->AddText(g_item, 40 * u, ImVec2(x + 40 * u, ty), A(kWhite), T("INSTALL COMPLETE", "INSTALAÇÃO CONCLUÍDA"));
    ty += 56 * u;
    dl->AddText(g_body, 26 * u, ImVec2(x + 40 * u, ty), A(kText), text.c_str(), nullptr,
                w - 80 * u);
    ty += th + 20 * u;
    dl->AddText(g_body, 24 * u, ImVec2(x + 40 * u, ty), A(kGrey), list.c_str(), nullptr, w - 80 * u);
    ty += lh + 30 * u;
    dl->AddText(g_body, 28 * u, ImVec2(x + 40 * u, ty), A(kBlue), T("X  DELETE THEM          O  KEEP THEM", "X  APAGAR          O  GUARDAR"));
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
    const int shown = page_ == kSettings ? set_game_ : game_;
    shown_game_ += (float(shown) - shown_game_) * std::min(1.0f, dt * 5.0f);
    for (int g = 1; g <= kCd3; ++g) {
      const Image& background = background_[g];
      const float weight = std::max(0.0f, 1.0f - std::fabs(shown_game_ - float(g)));
      if (!background || weight < 0.01f) continue;
      const float cover = std::max(size.x / background.w, size.y / background.h);
      const float s = cover * (1.10f + 0.035f * std::sin(time_ * 0.045f));
      const float w = background.w * s, h = background.h * s;
      const float dx = std::sin(time_ * 0.031f) * (w - size.x) * 0.42f;
      const float dy = std::cos(time_ * 0.023f) * (h - size.y) * 0.38f;
      const ImVec2 p0((size.x - w) * 0.5f + dx, (size.y - h) * 0.5f + dy);
      // (the fire's flicker: a little brighter and warmer now and then)
      const float flicker = 0.5f + 0.25f * std::sin(time_ * 7.3f) + 0.25f * std::sin(time_ * 12.1f + 1.7f);
      const int lit = int(205 + 25 * flicker);
      dl->AddImage(background.id, p0, ImVec2(p0.x + w, p0.y + h), ImVec2(0, 0), ImVec2(1, 1),
                   A(IM_COL32(255, std::min(255, lit + 10), lit, int(255 * weight))));
      dl->AddRectFilledMultiColor(ImVec2(0, size.y * 0.55f), size, A(IM_COL32(255, 110, 30, 0)),
                                  A(IM_COL32(255, 110, 30, 0)), A(IM_COL32(255, 90, 20, int(14 + 16 * flicker))),
                                  A(IM_COL32(255, 90, 20, int((14 + 16 * flicker) * weight))));
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
    dl->AddRectFilled(ImVec2(0, scan), ImVec2(size.x, scan + 2 * u), A(IM_COL32(70, 165, 255, 16)));
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
      const float w = Width(g_item, ts, PageName(i));
      if (i == page_) {
        tab_target_x_ = x, tab_target_w_ = w;
        if (tab_w_ == 0) tab_x_ = x, tab_w_ = w;
      }
      dl->AddText(g_item, ts, ImVec2(x, y), A(i == page_ ? kWhite : kGrey), PageName(i));
      x += w + 52 * u;
    }
    button(x - 22 * u, "R1");
    // the project's name at the right (hub/title.png if there is one)
    if (title_) {
      const float th = 46 * u, tw = title_.w * th / title_.h;
      dl->AddImage(title_.id, ImVec2(size.x - 140 * u - tw, y - 2 * u), ImVec2(size.x - 140 * u, y - 2 * u + th),
                   ImVec2(0, 0), ImVec2(1, 1), A(kWhite));
    } else {
      // "CRACKDOWN" and a letter-spaced green "REACTIVATED", as the credits' title
      float w = 0;
      for (const char* c = "REACTIVATED"; *c; ++c) w += g_item->CalcTextSizeA(24 * u, FLT_MAX, 0, c, c + 1).x + 3 * u;
      w -= 3 * u;
      const float cw = Width(g_item, 30 * u, "CRACKDOWN");
      const float right = size.x - 140 * u;
      dl->AddText(g_item, 30 * u, ImVec2(right - w - 14 * u - cw, y + 6 * u), A(kWhite), "CRACKDOWN");
      Spaced(dl, g_item, 24 * u, ImVec2(right - w, y + 11 * u), A(kBlue), "REACTIVATED", 3 * u);
    }
    dl->AddRectFilled(ImVec2(tab_x_, y + 56 * u), ImVec2(tab_x_ + tab_w_, y + 61 * u), A(kBlue));
    dl->AddRectFilled(ImVec2(140 * u, y + 61 * u), ImVec2(size.x - 140 * u, y + 62 * u), A(IM_COL32(255, 255, 255, 40)));
  }

  void DrawFooter(ImDrawList* dl, const ImVec2& size) {
    const float u = u_, y = size.y - 92 * u;
    std::string hints;
    switch (page_) {
      case kHome: hints = T("UP DOWN  GAME      X  PLAY      L1 R1  PAGES", "CIMA BAIXO  JOGO      X  JOGAR      L1 R1  PÁGINAS"); break;
      case kContent:
        hints = T("UP DOWN  MOVE      X  INSTALL      TRIANGLE  UNINSTALL      O  BACK      L1 R1  PAGES",
                  "CIMA BAIXO  MOVER      X  INSTALAR      TRIÂNGULO  DESINSTALAR      O  VOLTAR      L1 R1  PÁGINAS");
        break;
      case kSaves:
        hints = T("UP DOWN  MOVE      X  SELECT      O  BACK      L1 R1  PAGES",
                  "CIMA BAIXO  MOVER      X  ESCOLHER      O  VOLTAR      L1 R1  PÁGINAS");
        break;
      case kSettings:
        hints = in_list_ ? T("UP DOWN  MOVE      LEFT RIGHT  CHANGE      SQUARE  RECOMMENDED      O  GAME      L1 R1  PAGES", "CIMA BAIXO  MOVER      ESQUERDA DIREITA  MUDAR      QUADRADO  RECOMENDADO      O  JOGO      L1 R1  PÁGINAS")
                         : T("UP DOWN  GAME      X  SETTINGS      SQUARE  RECOMMENDED      O  BACK      L1 R1  PAGES", "CIMA BAIXO  JOGO      X  CONFIGURAÇÕES      QUADRADO  RECOMENDADO      O  VOLTAR      L1 R1  PÁGINAS");
        break;
      case kTrophies:
        hints = T("UP DOWN  MOVE      LEFT RIGHT  PAGE UP / DOWN      TRIANGLE  OTHER GAME      O  BACK",
                  "CIMA BAIXO  MOVER      ESQUERDA DIREITA  PULAR PÁGINA      TRIÂNGULO  OUTRO JOGO      O  VOLTAR");
        break;
      default: hints = T("O  BACK      L1 R1  PAGES", "O  VOLTAR      L1 R1  PÁGINAS"); break;
    }
    const float hints_w = Hints(dl, g_body, 22 * u, ImVec2(0, 0), 0, hints, false);
    // HOME leaves the picture undimmed: a shade in the corner under the hints,
    // fading out to the left and upward
    const float shade = 1.0f - std::clamp(home_dark_ / 0.78f, 0.0f, 1.0f);
    if (shade > 0.01f) {
      const float xs = size.x - 140 * u - hints_w - 40 * u, ys = size.y - 150 * u, f = 160 * u;
      const ImU32 c = A(IM_COL32(0, 0, 0, int(200 * shade))), z = A(IM_COL32(0, 0, 0, 0));
      dl->AddRectFilled(ImVec2(xs, ys), size, c);
      dl->AddRectFilledMultiColor(ImVec2(xs, ys - f), ImVec2(size.x, ys), z, z, c, c);
      dl->AddRectFilledMultiColor(ImVec2(xs - f, ys), ImVec2(xs, size.y), z, c, c, z);
      dl->AddRectFilledMultiColor(ImVec2(xs - f, ys - f), ImVec2(xs, ys), z, z, c, z);
    }
    dl->AddRectFilled(ImVec2(140 * u, y - 18 * u), ImVec2(size.x - 140 * u, y - 17 * u), A(IM_COL32(255, 255, 255, 40)));
    dl->AddText(g_body, 22 * u, ImVec2(140 * u, y), A(kBlue),
                Upper(Date(int64_t(std::time(nullptr)), T("%a / %b %d / %I:%M %p", "%a / %d %b / %H:%M"))).c_str());
    Hints(dl, g_body, 22 * u, ImVec2(size.x - 140 * u - hints_w, y), A(kBlue), hints, true);
  }

  void DrawHome(ImDrawList* dl, const ImVec2& size) {
    const float u = u_, left = 140 * u + page_dx_;
    float y = 220 * u;
    // the REACTIVATED wordmark over the list (the backgrounds have the games' own logos)
    if (logo_) {
      const float h = (logo_.w > 3 * logo_.h ? 80 : 110) * u, w = logo_.w * h / logo_.h;
      dl->AddImage(logo_.id, ImVec2(left, y), ImVec2(left + w, y + h), ImVec2(0, 0), ImVec2(1, 1), P(kWhite));
      y += h + 30 * u;
    }
    for (int g = 1; g <= kCd3; ++g) {
      const bool sel = g == game_;
      const float h = (sel ? 92 : 50) * u;
      if (sel) {
        const float pulse = 0.7f + 0.3f * std::sin(time_ * 5.0f);
        dl->AddRectFilled(ImVec2(left - 38 * u, y + h * 0.18f), ImVec2(left - 24 * u, y + h * 0.86f),
                          P((kBlue & 0x00FFFFFFu) | (ImU32(255 * pulse) << 24)));
      }
      dl->AddText(g_title, h, ImVec2(left, y - h * 0.1f), P(sel ? kWhite : kGrey), kGameTitles[g]);
      y += h + (sel ? 14 : 26) * u;
      if (!sel) continue;
      const bool installing = install::TheJob().Busy();
      const float ps = 72 * u;
      const char* action = g == kCd3 ? Cd3Action() : T("PLAY", "JOGAR");
      const bool lit = g == kCd3 ? Cd3ActionLit() : Ready() && !installing;
      dl->AddText(g_item, ps, ImVec2(left, y), P(lit ? kWhite : kDim), action);
      y += ps + 20 * u;
      if (!start_error_.empty()) {
        const float pw = 900 * u, th = g_body->CalcTextSizeA(26 * u, FLT_MAX, pw, start_error_.c_str()).y;
        dl->AddRectFilled(ImVec2(left - 16 * u, y - 12 * u), ImVec2(left + pw + 16 * u, y + th + 12 * u),
                          P(IM_COL32(40, 6, 6, 230)), 6 * u);
        dl->AddText(g_body, 26 * u, ImVec2(left, y), P(kWhite), start_error_.c_str(), nullptr, pw);
        y += th + 36 * u;
      }
      if (g == kCd3) {
        y = DrawCd3(dl, left, y);
      } else if (installing) {
        y = DrawJob(dl, left, y, 900 * u) + 20 * u;
      } else if (!Ready()) {
        // what's missing, and where it goes
        missing_flash_ = std::max(0.0f, missing_flash_ - 0.03f);
        std::string what_text;
        if (g == kCd3)
          what_text = !cd3_.prospero
                          ? T("PROSPERO WIN ISN'T INSTALLED\nCrackdown 3 runs through Prospero Win (PPSA99995). Install "
                              "it, then set up Crackdown 3.",
                              "O PROSPERO WIN NÃO ESTÁ INSTALADO\nO Crackdown 3 roda pelo Prospero Win (PPSA99995). "
                              "Instale-o e depois prepare o Crackdown 3.")
                          : T("CRACKDOWN 3 ISN'T SET UP\nIts Prospero Win profile or game files are missing "
                              "(/data/prospero-win/profiles/cd3.profile, /mnt/ext1/cd3/stage/cd3).",
                              "O CRACKDOWN 3 NÃO ESTÁ PREPARADO\nFaltam o perfil do Prospero Win ou os arquivos do jogo "
                              "(/data/prospero-win/profiles/cd3.profile, /mnt/ext1/cd3/stage/cd3).");
        else if (g == 1)
          what_text = !files_.xex
                          ? T("THE GAME ISN'T INSTALLED\nPut your Crackdown disc image (.iso) or the disc's files in "
                              "PPSA53070/install/ and open the app again: it installs by itself. The CONTENT page "
                              "shows what's there.",
                              "O JOGO NÃO ESTÁ INSTALADO\nColoque a imagem do disco de Crackdown (.iso) ou os arquivos "
                              "do disco em PPSA53070/install/ e abra o app de novo: ele se instala sozinho. A página "
                              "CONTEÚDO mostra o que está lá.")
                          : T("TITLE UPDATE 6 ISN'T INSTALLED\nPut Title Update 6's package (TU_16L61US_...1G1) in "
                              "PPSA53070/install/ and open the app again: it installs by itself.",
                              "A TITLE UPDATE 6 NÃO ESTÁ INSTALADA\nColoque o pacote da Title Update 6 "
                              "(TU_16L61US_...1G1) em PPSA53070/install/ e abra o app de novo: ela se instala "
                              "sozinha.");
        else
          what_text = !installed_.game2
                          ? T("CRACKDOWN 2 ISN'T INSTALLED\nPut your Crackdown 2 disc image (.iso, any name) or the disc's "
                              "files, and its Title Update 5 package, in PPSA53070/install/ and open the app again: they "
                              "install by themselves. The CONTENT page shows what's there.",
                              "O CRACKDOWN 2 NÃO ESTÁ INSTALADO\nColoque a imagem do disco de Crackdown 2 (.iso, qualquer "
                              "nome) ou os arquivos do disco, e o pacote da Title Update 5, em PPSA53070/install/ e abra o "
                              "app de novo: eles se instalam sozinhos. A página CONTEÚDO mostra o que está lá.")
                          : T("CRACKDOWN 2 NEEDS TITLE UPDATE 5\nPut Crackdown 2's Title Update 5 package "
                              "(TU_16L625S_...181) in PPSA53070/install/ and open the app again: it installs by itself.",
                              "O CRACKDOWN 2 PRECISA DA TITLE UPDATE 5\nColoque o pacote da Title Update 5 do Crackdown 2 "
                              "(TU_16L625S_...181) em PPSA53070/install/ e abra o app de novo: ela se instala sozinha.");
        what_text = Here(what_text);
        const char* what = what_text.c_str();
        const float pw = 900 * u, th = g_body->CalcTextSizeA(26 * u, FLT_MAX, pw, what).y;
        dl->AddRectFilled(ImVec2(left - 16 * u, y - 12 * u), ImVec2(left + pw + 16 * u, y + th + 12 * u),
                          P(IM_COL32(40, 6, 6, ImU32(200 + 55 * missing_flash_))), 6 * u);
        dl->AddRectFilled(ImVec2(left - 16 * u, y - 12 * u), ImVec2(left - 10 * u, y + th + 12 * u),
                          P(IM_COL32(230, 50, 40, 255)));
        dl->AddText(g_body, 26 * u, ImVec2(left, y), P(kWhite), what, nullptr, pw);
        y += th + 42 * u;
      } else if (g == kCd3) {
        dl->AddText(g_body, 26 * u, ImVec2(left, y), P(kGrey),
                    T("Runs through Prospero Win  -  your Xbox account signs in in the game.",
                      "Roda pelo Prospero Win  -  sua conta Xbox entra no jogo."));
        y += 54 * u;
      } else {
        char line[200];
        const int dlc = g == 2 ? installed_.dlc2 : files_.dlc;
        const std::string packs = dlc == 1 ? T("1 DLC pack", "1 pacote de DLC")
                                           : std::to_string(dlc) + T(" DLC packs", " pacotes de DLC");
        if (g == 2)
          std::snprintf(line, sizeof(line),
                        T("Title Update 5  -  %s  -  your saves load with the game.",
                          "Title Update 5  -  %s  -  seus saves carregam com o jogo."),
                        packs.c_str());
        else
          std::snprintf(line, sizeof(line),
                        T("Title Update 6%s  -  %s  -  your saves load with the game.",
                          "Title Update 6%s  -  %s  -  seus saves carregam com o jogo."),
                        files_.update ? "" : T(" (without its text files)", " (sem os arquivos de texto)"), packs.c_str());
        dl->AddText(g_body, 26 * u, ImVec2(left, y), P(kGrey), line);
        y += 54 * u;
      }
      const trophies::List& list = lists_[std::min(g, 3)];
      if (g <= 3 && list.loaded) {
        // (on a dark panel: the picture behind it is busy)
        dl->AddRectFilled(ImVec2(left - 16 * u, y - 12 * u), ImVec2(left + 560 * u, y + 48 * u),
                          P(IM_COL32(8, 8, 8, 185)), 6 * u);
        dl->AddRectFilled(ImVec2(left - 16 * u, y - 12 * u), ImVec2(left - 11 * u, y + 48 * u), P(kBlue));
        trophies::DrawCup(dl, ImVec2(left + 14 * u, y + 16 * u), 30 * u,
                          list.Platinum() ? trophies::Grade::kPlatinum : trophies::Grade::kGold, page_alpha_,
                          list.earned > 0);
        char line[96];
        std::snprintf(line, sizeof(line), T("%d of %d trophies  -  %d%%", "%d de %d troféus  -  %d%%"), list.earned,
                      list.earnable, list.Percent());
        dl->AddText(g_body, 26 * u, ImVec2(left + 40 * u, y), P(kWhite), line);
        y += 76 * u;
      }
      y += 30 * u;
    }
  }

  // The running job's title and progress bar, or the last one's outcome for a while; the y under it.
  // ---- Crackdown 3 (cd3_setup.h): X installs it, then signs in to Xbox, then plays ----
  enum class Cd3Step { kNeedProspero, kInstall, kNeedFiles, kSignIn, kPlay };
  Cd3Step Cd3Next() const {
    if (!cd3_.prospero) return Cd3Step::kNeedProspero;
    if (!cd3_.installed) return cd3_.dump && cd3_.windows && cd3_.data ? Cd3Step::kInstall : Cd3Step::kNeedFiles;
    if (!cd3_.signed_in) return Cd3Step::kSignIn;
    return Cd3Step::kPlay;
  }
  const char* Cd3Action() const {
    if (cd3_job_.Active())
      return cd3_job_.kind == cd3::Job::Kind::kSignIn  ? T("SIGNING IN", "ENTRANDO")
             : cd3_job_.kind == cd3::Job::Kind::kLaunch ? T("STARTING", "INICIANDO")
                                                        : T("INSTALLING", "INSTALANDO");
    switch (Cd3Next()) {
      case Cd3Step::kInstall: return T("INSTALL", "INSTALAR");
      case Cd3Step::kSignIn: return T("SIGN IN TO XBOX", "ENTRAR NO XBOX");
      default: return T("PLAY", "JOGAR");
    }
  }
  bool Cd3ActionLit() const {
    if (cd3_job_.Active()) return false;
    const Cd3Step s = Cd3Next();
    return s == Cd3Step::kInstall || s == Cd3Step::kSignIn || s == Cd3Step::kPlay;
  }
  void Cd3Cross() {
    Refresh();  // (files copied in since the menu opened?)
    if (cd3_job_.Active()) return;
    cd3_error_.clear();
    const Cd3Step s = Cd3Next();
    if (s == Cd3Step::kInstall || s == Cd3Step::kSignIn) {
      std::string error;
      if (!cd3::Start(app_, s == Cd3Step::kInstall ? cd3::Job::Kind::kInstall : cd3::Job::Kind::kSignIn, cd3_job_,
                      error))
        cd3_error_ = error;
      return;
    }
    missing_flash_ = 1.0f;
  }
  // A panel of text (red: something's missing or failed).
  float Cd3Panel(ImDrawList* dl, float left, float y, const std::string& text, bool red) {
    const float u = u_, pw = 900 * u, th = g_body->CalcTextSizeA(26 * u, FLT_MAX, pw, text.c_str()).y;
    dl->AddRectFilled(ImVec2(left - 16 * u, y - 12 * u), ImVec2(left + pw + 16 * u, y + th + 12 * u),
                      P(red ? IM_COL32(40, 6, 6, ImU32(200 + 55 * missing_flash_)) : IM_COL32(8, 8, 8, 200)), 6 * u);
    dl->AddRectFilled(ImVec2(left - 16 * u, y - 12 * u), ImVec2(left - 10 * u, y + th + 12 * u),
                      P(red ? IM_COL32(230, 50, 40, 255) : kBlue));
    dl->AddText(g_body, 26 * u, ImVec2(left, y), P(kWhite), text.c_str(), nullptr, pw);
    return y + th + 42 * u;
  }
  float DrawCd3(ImDrawList* dl, float left, float y) {
    const float u = u_;
    missing_flash_ = std::max(0.0f, missing_flash_ - 0.03f);
    if (!cd3_error_.empty()) y = Cd3Panel(dl, left, y, cd3_error_, true);
    const cd3::Job& j = cd3_job_;
    if (j.kind == cd3::Job::Kind::kSignIn && j.phase == "code" && !j.code.empty()) {
      // the code, and a QR code of the link with the code in it
      if (cd3_qr_text_ != j.url) {
        cd3_qr_text_ = j.url;
        cd3_qr_ = cd3::QrModules(j.url, cd3_qr_size_);
      }
      const float side = 300 * u, border = 3;
      dl->AddRectFilled(ImVec2(left - 16 * u, y - 12 * u), ImVec2(left + 900 * u, y + side + 24 * u),
                        P(IM_COL32(8, 8, 8, 215)), 6 * u);
      if (cd3_qr_size_) {
        const float m = std::max(1.0f, std::floor(side / float(cd3_qr_size_ + border * 2)));
        const float full = m * float(cd3_qr_size_ + border * 2);
        dl->AddRectFilled(ImVec2(left, y), ImVec2(left + full, y + full), P(IM_COL32(255, 255, 255, 255)));
        for (int r = 0; r < cd3_qr_size_; ++r)
          for (int c = 0; c < cd3_qr_size_; ++c)
            if (cd3_qr_[size_t(r) * size_t(cd3_qr_size_) + size_t(c)])
              dl->AddRectFilled(ImVec2(left + (c + border) * m, y + (r + border) * m),
                                ImVec2(left + (c + border + 1) * m, y + (r + border + 1) * m),
                                P(IM_COL32(0, 0, 0, 255)));
      }
      const float tx = left + side + 40 * u;
      dl->AddText(g_body, 28 * u, ImVec2(tx, y + 6 * u), P(kWhite),
                  T("SIGN IN ON YOUR PHONE", "ENTRE PELO CELULAR"));
      dl->AddText(g_body, 24 * u, ImVec2(tx, y + 50 * u), P(kGrey),
                  T("Scan the code, or go to microsoft.com/link\nand enter:",
                    "Leia o código, ou vá a microsoft.com/link\ne digite:"),
                  nullptr, 520 * u);
      dl->AddText(g_title, 76 * u, ImVec2(tx, y + 120 * u), P(kBlue), j.code.c_str());
      dl->AddText(g_body, 22 * u, ImVec2(tx, y + 220 * u), P(kGrey),
                  T("Use the Microsoft account that owns Crackdown 3.  O stops.",
                    "Use a conta Microsoft que tem o Crackdown 3.  O cancela."),
                  nullptr, 520 * u);
      return y + side + 60 * u;
    }
    if (j.Active()) {
      std::string line = j.message.empty() ? std::string(T("Working...", "Trabalhando...")) : j.message;
      if (j.total) {
        char pct[32];
        std::snprintf(pct, sizeof(pct), "  -  %d%%", int(std::min<uint64_t>(100, j.done * 100 / j.total)));
        line += pct;
      }
      y = Cd3Panel(dl, left, y, line, false);
      if (j.total) {
        const float w = 900 * u, f = std::min(1.0f, float(double(j.done) / double(j.total)));
        dl->AddRectFilled(ImVec2(left, y - 30 * u), ImVec2(left + w, y - 22 * u), P(IM_COL32(40, 40, 40, 220)));
        dl->AddRectFilled(ImVec2(left, y - 30 * u), ImVec2(left + w * f, y - 22 * u), P(kBlue));
      }
      return y;
    }
    if (j.phase == "failed") y = Cd3Panel(dl, left, y, j.error, true);
    switch (Cd3Next()) {
      case Cd3Step::kNeedProspero:
        return Cd3Panel(dl, left, y,
                        T("PROSPERO WIN IS REQUIRED\nCrackdown 3 runs through Prospero Win. Install Prospero Win from "
                          "the Recomp store, then come back here.",
                          "O PROSPERO WIN É NECESSÁRIO\nO Crackdown 3 roda pelo Prospero Win. Instale o Prospero Win "
                          "pela Recomp store e volte aqui."),
                        true);
      case Cd3Step::kNeedFiles: {
        std::string t = T("CRACKDOWN 3 ISN'T INSTALLED\n", "O CRACKDOWN 3 NÃO ESTÁ INSTALADO\n");
        if (!cd3_.data)
          t += T("This app's Crackdown 3 files are missing: reinstall the Crackdown app.",
                 "Faltam os arquivos do Crackdown 3 deste app: reinstale o app do Crackdown.");
        else {
          if (!cd3_.dump)
            t += T("- Your game: dump your own Crackdown 3 (Store / Game Pass) with UWPDumper on a PC and put the "
                   "folder it makes in PPSA53070/install/.\n",
                   "- Seu jogo: extraia o seu Crackdown 3 (Store / Game Pass) com o UWPDumper num PC e coloque a "
                   "pasta que ele cria em PPSA53070/install/.\n");
          if (!cd3_.windows)
            t += T("- The Windows files: run the PC helper (cd3_windows_files.py) and put its folder, \"Crackdown 3 "
                   "Windows files\", in PPSA53070/install/.",
                   "- Os arquivos do Windows: rode o ajudante de PC (cd3_windows_files.py) e coloque a pasta dele, "
                   "\"Crackdown 3 Windows files\", em PPSA53070/install/.");
        }
        return Cd3Panel(dl, left, y, Here(t), true);
      }
      case Cd3Step::kInstall:
        return Cd3Panel(dl, left, y,
                        T("Your Crackdown 3 files are in install/.  X installs them (a few minutes): Prospero Win has to "
                          "be closed.",
                          "Seus arquivos do Crackdown 3 estão em install/.  X instala (alguns minutos): o Prospero Win "
                          "precisa estar fechado."),
                        false);
      case Cd3Step::kSignIn:
        return Cd3Panel(dl, left, y,
                        T("Installed.  X signs in to Xbox once, with a code on your phone: Crackdown 3 needs it to "
                          "start.",
                          "Instalado.  X entra no Xbox uma vez, com um código no celular: o Crackdown 3 precisa disso "
                          "para começar."),
                        false);
      case Cd3Step::kPlay:
        dl->AddText(g_body, 26 * u, ImVec2(left, y), P(kGrey),
                    T("Runs through Prospero Win  -  signed in to Xbox.", "Roda pelo Prospero Win  -  conectado ao Xbox."));
        return y + 54 * u;
    }
    return y;
  }

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
      dl->AddText(g_body, 26 * u, ImVec2(x + w - pw, y), P(kBlue), line);
      dl->AddRectFilled(ImVec2(x, y + 46 * u), ImVec2(x + w, y + 54 * u), P(kDim));
      dl->AddRectFilled(ImVec2(x, y + 46 * u), ImVec2(x + w * pr, y + 54 * u), P(kBlue));
      return y + 90 * u;
    }
    const std::string result = job.Result();
    if (!result.empty() && time_ - result_t_ < 12.0f) {
      const float th = g_body->CalcTextSizeA(26 * u, FLT_MAX, w, result.c_str()).y;
      dl->AddRectFilled(ImVec2(x - 16 * u, y - 12 * u), ImVec2(x + w + 16 * u, y + th + 12 * u),
                        P(IM_COL32(8, 8, 8, 200)), 6 * u);
      dl->AddRectFilled(ImVec2(x - 16 * u, y - 12 * u), ImVec2(x - 10 * u, y + th + 12 * u),
                        P(job.Failed() ? IM_COL32(230, 50, 40, 255) : kBlue));
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
    const std::string dlc2_name =
        installed_.dlc2 ? std::string(T("CRACKDOWN 2 DLC  (", "DLC DO CRACKDOWN 2  (")) + std::to_string(installed_.dlc2) + ")"
                        : std::string(T("CRACKDOWN 2 DLC", "DLC DO CRACKDOWN 2"));
    const std::string names[kContentRows] = {T("BASE GAME", "JOGO BASE"), "TITLE UPDATE 6", install::DlcName(0),
                                             install::DlcName(1), "CRACKDOWN 2",
                                             T("CRACKDOWN 2 TITLE UPDATE 5", "TITLE UPDATE 5 DO CRACKDOWN 2"), dlc2_name};
    const char* help[kContentRows] = {
        T("Crackdown itself. Put your disc image (.iso, any name) or a folder of the disc's files in "
          "PPSA53070/install/: it installs into PPSA53070/game/ when the app opens, or with X here. Afterwards the "
          "app asks whether to delete the image.",
          "O próprio Crackdown. Coloque a imagem do disco (.iso, qualquer nome) ou uma pasta com os arquivos do disco "
          "em PPSA53070/install/: ele se instala na pasta game do app quando o app abre, ou com X aqui. Depois o app "
          "pergunta se pode apagar a imagem."),
        T("Required: the game runs Title Update 6's code. Put its package (TU_16L61US_0000004000000.00000000001G1, "
          "any name) in PPSA53070/install/. Other title updates can't be used.",
          "Obrigatória: o jogo roda o código da Title Update 6. Coloque o pacote dela "
          "(TU_16L61US_0000004000000.00000000001G1, qualquer nome) em PPSA53070/install/. Outras atualizações não "
          "servem."),
        T("Optional DLC (new vehicles, weapons and modes). Put its package in PPSA53070/install/.",
          "DLC opcional (novos veículos, armas e modos). Coloque o pacote dela em PPSA53070/install/."),
        T("Optional DLC (new vehicles, weapons and modes). Put its package in PPSA53070/install/.",
          "DLC opcional (novos veículos, armas e modos). Coloque o pacote dela em PPSA53070/install/."),
        T("Crackdown 2 itself. Put your disc image (.iso, any name) or a folder of the disc's files in "
          "PPSA53070/install/: it installs into the app's game2 folder when the app opens, or with X here.",
          "O próprio Crackdown 2. Coloque a imagem do disco (.iso, qualquer nome) ou uma pasta com os arquivos do "
          "disco em PPSA53070/install/: ele se instala na pasta game2 do app quando o app abre, ou com X aqui."),
        T("Required for Crackdown 2: its program is Title Update 5's. Put its package "
          "(TU_16L625S_0000004000000.0000000000181, any name) in PPSA53070/install/.",
          "Obrigatória para o Crackdown 2: o programa dele é o da Title Update 5. Coloque o pacote "
          "(TU_16L625S_0000004000000.0000000000181, qualquer nome) em PPSA53070/install/."),
        T("Optional: Crackdown 2's DLC (the Toy Box, Deluge and Pre-Order Armor packs). Put their packages "
          "(any names) in PPSA53070/install/: each installs by itself. TRIANGLE removes them all.",
          "Opcional: as DLCs do Crackdown 2 (os pacotes Toy Box, Deluge e Pre-Order Armor). Coloque os pacotes "
          "(qualquer nome) em PPSA53070/install/: cada um se instala sozinho. TRIÂNGULO remove todos.")};
    for (int r = 0; r < kContentRows; ++r) {
      const bool sel = r == content_row_, in = RowInstalled(r);
      const install::Source* src = SourceFor(r);
      const float fs = 34 * u;
      if (r == kRowGame2) {
        // (Crackdown 2's rows under a line of their own)
        dl->AddRectFilled(ImVec2(left, y - 4 * u), ImVec2(left + 820 * u, y - 3 * u), P(IM_COL32(255, 255, 255, 40)));
        y += 10 * u;
      }
      if (sel) dl->AddRectFilled(ImVec2(left - 30 * u, y + 7 * u), ImVec2(left - 20 * u, y + fs), P(kBlue));
      dl->AddText(g_item, fs, ImVec2(left, y), P(sel ? kWhite : kGrey), names[r].c_str());
      std::string status = in && !(r == kRowDlc2 && src) ? T("INSTALLED", "INSTALADO")
                           : src ? T("READY TO INSTALL", "PRONTO PARA INSTALAR")
                                 : T("NOT INSTALLED", "NÃO INSTALADO");
      if (r == kRowDlc2 && src)
        status = std::to_string(Dlc2Waiting()) + T(" READY TO INSTALL", " PRONTOS PARA INSTALAR");
      if (sel && Confirming(100 + r))
        status = T("PRESS TRIANGLE AGAIN TO UNINSTALL", "TRIÂNGULO DE NOVO: DESINSTALAR");
      const bool required = r == kRowGame || r == kRowUpdate || r == kRowGame2 || r == kRowUpdate2;
      dl->AddText(g_body, 26 * u, ImVec2(left + 470 * u, y + 6 * u),
                  P(in ? kBlue : src ? kWhite : (required ? IM_COL32(230, 90, 70, 255) : kGrey)), status.c_str());
      y += fs + 4 * u;
      if (src && !in) {
        dl->AddText(g_body, 22 * u, ImVec2(left + 470 * u, y), P(kGrey), Short(src->label, 40).c_str());
        y += 28 * u;
      }
      y += 12 * u;
    }
    y += 10 * u;
    y = DrawJob(dl, left, y, 820 * u) + 10 * u;
    // the rest of install/: what can't be installed, and why
    std::vector<std::string> notes;
    int avatar_items = 0;
    for (const auto& s : sources_) {
      if (s.kind == install::Kind::kArchive)
        notes.push_back(Short(s.label, 40) + T("  -  extract it on a PC first (7-Zip)", "  -  extraia num PC antes (7-Zip)"));
      else if (s.kind == install::Kind::kOtherUpdate)
        notes.push_back(Short(s.label, 40) + T("  -  not Title Update 6", "  -  não é a Title Update 6"));
      else if (s.kind == install::Kind::kOther && s.label == install::kAvatarItemLabel)
        ++avatar_items;
      else if (s.kind == install::Kind::kOther)
        notes.push_back(Short(s.label, 60));
    }
    if (avatar_items)
      notes.push_back(std::to_string(avatar_items) +
                      (avatar_items == 1 ? T(" Xbox 360 avatar item", " item de avatar do Xbox 360")
                                         : T(" Xbox 360 avatar items", " itens de avatar do Xbox 360")) +
                      T("  -  not game DLC, not used here", "  -  não são DLC do jogo, não usados aqui"));
    if (!notes.empty()) {
      dl->AddText(g_body, 20 * u, ImVec2(left, y), P(kBlue), T("ALSO IN THE INSTALL FOLDER", "TAMBÉM NA PASTA DE INSTALAÇÃO"));
      y += 34 * u;
      for (size_t i = 0; i < notes.size() && i < 5; ++i, y += 32 * u)
        dl->AddText(g_body, 24 * u, ImVec2(left, y), P(kGrey), notes[i].c_str());
    }
    // the focused row's help, at the right
    const float px = 1180 * u + page_dx_, pw = size.x - 140 * u - px;
    dl->AddRectFilled(ImVec2(px - 40 * u, 200 * u), ImVec2(px - 39 * u, 760 * u), P(IM_COL32(255, 255, 255, 40)));
    dl->AddText(g_body, 26 * u, ImVec2(px, 204 * u), P(kText),
                Here(help[content_row_]).c_str(), nullptr, pw);
  }

  void DrawSaves(ImDrawList* dl, const ImVec2& size) {
    const float u = u_, left = 140 * u + page_dx_;
    float y = 200 * u;
    char count[128];
    std::snprintf(count, sizeof(count), "CRACKDOWN  %d %s      CRACKDOWN 2  %d %s", installed_.saves,
                  installed_.saves == 1 ? "SAVE" : "SAVES", installed_.saves2, installed_.saves2 == 1 ? "SAVE" : "SAVES");
    dl->AddText(g_body, 22 * u, ImVec2(left, y), P(kBlue), count);
    y += 48 * u;
    const auto saves = SaveSources();
    const int rows = 1 + int(saves.size());
    saves_row_ = std::min(saves_row_, rows - 1);
    for (int r = 0; r < rows; ++r) {
      const bool sel = r == saves_row_;
      const float fs = 36 * u;
      if (sel) dl->AddRectFilled(ImVec2(left - 30 * u, y + 6 * u), ImVec2(left - 20 * u, y + fs), P(kBlue));
      std::string label;
      if (r == 0) {
        label = T("BACK UP SAVES", "FAZER BACKUP DOS SAVES");
      } else {
        const install::Source* s = saves[size_t(r - 1)];
        label = (s->kind == install::Kind::kBackup ? T("RESTORE  ", "RESTAURAR  ") : T("IMPORT  ", "IMPORTAR  ")) +
                Short(s->label, 40);
        if (sel && Confirming(200 + r)) label = T("PRESS X AGAIN TO CONFIRM", "APERTE X DE NOVO PARA CONFIRMAR");
      }
      dl->AddText(g_item, fs, ImVec2(left, y), P(sel ? kWhite : kGrey), label.c_str());
      y += fs + 22 * u;
    }
    y += 10 * u;
    y = DrawJob(dl, left, y, 820 * u) + 10 * u;
    // the saves the app has, newest first
    if (!installed_.save_list.empty()) {
      Spaced(dl, g_body, 19 * u, ImVec2(left, y), P(kBlue), T("YOUR SAVES", "SEUS SAVES"), 3 * u);
      y += 36 * u;
      for (size_t i = 0; i < installed_.save_list.size() && i < 6; ++i, y += 34 * u) {
        const auto& sv = installed_.save_list[i];
        std::string name = sv.name;
        std::replace(name.begin(), name.end(), '_', ' ');
        dl->AddText(g_body, 26 * u, ImVec2(left, y), P(kWhite), Short((sv.game == 2 ? "CD2  " : "CD1  ") + name, 34).c_str());
        if (sv.modified)
          dl->AddText(g_body, 26 * u, ImVec2(left + 520 * u, y), P(kGrey),
                      Upper(Date(sv.modified, T("%b %d %Y  %I:%M %p", "%d %b %Y  %H:%M"))).c_str());
      }
    }
    const char* help =
        saves_row_ == 0
            ? T("Copies your saves, your trophies and your settings to PPSA53070/install/backups/. Copy that folder "
                "to a PC or a USB drive before deleting or reinstalling the app.",
                "Copia seus saves, seus troféus e seus ajustes para PPSA53070/install/backups/. Copie essa pasta "
                "para um PC ou um pendrive antes de apagar ou reinstalar o app.")
        : saves[size_t(saves_row_ - 1)]->kind == install::Kind::kBackup
            ? T("Puts this backup's saves, trophies and settings back, over the ones here.",
                "Devolve os saves, troféus e ajustes deste backup, por cima dos que estão aqui.")
            : T("Imports this Xbox 360 save (a package copied off a 360, with a USB drive or Horizon) into the app. "
                "A save of the same name here is replaced.",
                "Importa este save do Xbox 360 (um pacote copiado de um 360, com um pendrive ou o Horizon) para o "
                "app. Um save com o mesmo nome aqui é substituído.");
    const char* where =
        T("\n\nTo import a save, put the Xbox 360 save package in PPSA53070/install/. Backups made here are listed "
          "to restore.",
          "\n\nPara importar um save, coloque o pacote de save do Xbox 360 em PPSA53070/install/. Os backups feitos "
          "aqui aparecem para restaurar.");
    const std::string text = Here(std::string(help) + (saves.empty() ? where : ""));
    const float px = 1180 * u + page_dx_, pw = size.x - 140 * u - px;
    dl->AddRectFilled(ImVec2(px - 40 * u, 200 * u), ImVec2(px - 39 * u, 760 * u), P(IM_COL32(255, 255, 255, 40)));
    dl->AddText(g_body, 26 * u, ImVec2(px, 204 * u), P(kText), text.c_str(), nullptr, pw);
  }

  // SETTINGS' left side: the two games, as on HOME (the shown one large, with the accent's bar, pulsing
  // while the pad is on the list), its settings at the right (Saints Row: ReInstated's).
  void DrawSettingsGame(ImDrawList* dl, float left, float top) {
    const float u = u_;
    Spaced(dl, g_body, 19 * u, ImVec2(left, top), P(kBlue), T("GAMES", "JOGOS"), 3 * u);
    float y = top + 46 * u;
    for (int g = 1; g <= 2; ++g) {
      const bool sel = g == set_game_;
      const float h = (sel ? 53 : 37) * u;
      if (sel) {
        const float pulse = in_list_ ? 1.0f : 0.7f + 0.3f * std::sin(time_ * 5.0f);
        dl->AddRectFilled(ImVec2(left - 38 * u, y + h * 0.12f), ImVec2(left - 24 * u, y + h * 0.88f),
                          P((kBlue & 0x00FFFFFFu) | (ImU32(255 * pulse) << 24)));
      }
      dl->AddText(g_item, h * 0.8f, ImVec2(left, y), P(sel ? kWhite : kGrey), kGameTitles[g]);
      y += h + 26 * u;
    }
  }

  void DrawSettings(ImDrawList* dl, const ImVec2& size) {
    const float u = u_, left = 580 * u + page_dx_;
    const auto lines = SettingsLines();
    const float fs = 36 * u, top = 190 * u, bottom = size.y - 120 * u;
    const float px = 1360 * u + page_dx_, pw = size.x - 140 * u - px;  // (the help, at the right)
    DrawSettingsGame(dl, 140 * u + page_dx_, top);
    dl->AddRectFilled(ImVec2(left - 60 * u, 190 * u), ImVec2(left - 59 * u, 760 * u), P(IM_COL32(255, 255, 255, 40)));
    // where each row and section heading goes (scrolled so the focused row stays in view)
    std::vector<float> row_y(kSettingsRows, 0.0f), head_y(kSettingsRows, -1.0f);
    float y = 0;
    for (int r = 0; r < kSettingsRows; ++r) {
      if (!r || std::strcmp(lines[size_t(r - 1)].section, lines[size_t(r)].section) != 0) {
        if (r) y += 14 * u;
        head_y[size_t(r)] = y;
        y += 36 * u;
      }
      row_y[size_t(r)] = y;
      y += fs + 16 * u;
    }
    const float view = bottom - top;
    const float want = std::clamp(row_y[size_t(row_)] - view * 0.45f, 0.0f, std::max(0.0f, y - view));
    settings_scroll_ += (want - settings_scroll_) * 0.25f;
    dl->PushClipRect(ImVec2(0, top - 4 * u), ImVec2(px - 40 * u, bottom), true);
    for (int r = 0; r < kSettingsRows; ++r) {
      const SettingLine& l = lines[size_t(r)];
      if (head_y[size_t(r)] >= 0)
        Spaced(dl, g_body, 19 * u, ImVec2(left, top + head_y[size_t(r)] - settings_scroll_), P(kBlue), l.section,
               3 * u);
      y = top + row_y[size_t(r)] - settings_scroll_;
      const bool sel = in_list_ && r == row_;
      const ImU32 col = !l.enabled ? kDim : sel ? kWhite : kGrey;
      if (sel) dl->AddRectFilled(ImVec2(left - 30 * u, y + 8 * u), ImVec2(left - 20 * u, y + fs), P(kBlue));
      dl->AddText(g_item, fs, ImVec2(left, y), P(col), l.label);
      const std::string v = l.slider || !l.enabled ? l.value : "<  " + l.value + "  >";
      dl->AddText(g_item, fs, ImVec2(left + 380 * u, y), P(sel ? (l.enabled ? kBlue : kDim) : col), v.c_str());
      if (l.slider) {
        const float x0 = left + 480 * u, w = 220 * u, yy = y + fs * 0.6f;
        dl->AddRectFilled(ImVec2(x0, yy - 3 * u), ImVec2(x0 + w, yy + 3 * u), P(kDim));
        dl->AddRectFilled(ImVec2(x0, yy - 3 * u), ImVec2(x0 + w * Shown().music / 100.0f, yy + 3 * u),
                          P(sel ? kBlue : kGrey));
      }
    }
    dl->PopClipRect();
    // the focused row's help, at the right (on the game: what it's for)
    dl->AddRectFilled(ImVec2(px - 40 * u, 190 * u), ImVec2(px - 39 * u, 760 * u), P(IM_COL32(255, 255, 255, 40)));
    dl->AddText(g_item, 34 * u, ImVec2(px, 190 * u), P(kWhite), in_list_ ? lines[size_t(row_)].label : kGameTitles[set_game_]);
    const char* about =
        set_game_ == 2
            ? T("Crackdown 2's own settings. X or RIGHT to change them. SQUARE puts back the recommended settings.",
                "Configurações próprias de Crackdown 2. X ou DIREITA para mudá-las. QUADRADO volta às configurações recomendadas.")
            : T("Crackdown's settings. X or RIGHT to change them. SQUARE puts back the recommended settings, the ones it was tested with.",
                "Configurações de Crackdown. X ou DIREITA para mudá-las. QUADRADO volta às configurações recomendadas, as que foram testadas.");
    dl->AddText(g_body, 26 * u, ImVec2(px, 244 * u), P(kText), in_list_ ? lines[size_t(row_)].help.c_str() : about, nullptr, pw);
    if (Confirming(300 + set_game_))
      dl->AddText(g_body, 22 * u, ImVec2(px, 676 * u), P(kBlue),
                  T("PRESS SQUARE AGAIN TO PUT BACK THE RECOMMENDED SETTINGS", "APERTE QUADRADO DE NOVO PARA VOLTAR ÀS CONFIGURAÇÕES RECOMENDADAS"), nullptr, pw);
    else if (time_ - reset_t_ < 3.0f)
      dl->AddText(g_body, 22 * u, ImVec2(px, 700 * u), P(kBlue),
                  T("RECOMMENDED SETTINGS PUT BACK", "CONFIGURAÇÕES RECOMENDADAS RESTAURADAS"));
    else
      dl->AddText(g_body, 22 * u, ImVec2(px, 700 * u), P(kDim), T("Saved when you press PLAY.", "Salvo quando você aperta JOGAR."));
  }

  void DrawTrophies(ImDrawList* dl, const ImVec2& size, float dt) {
    const float u = u_;
    // which game's, at the top right (TRIANGLE: the other's)
    {
      const std::string which = std::string(kGameTitles[trophy_game_]) + T("  /  TRIANGLE: ", "  /  TRIÂNGULO: ") +
                                kGameTitles[trophy_game_ % 3 + 1];
      const float ww = Hints(dl, g_body, 22 * u, ImVec2(0, 0), 0, which, false);
      Hints(dl, g_body, 22 * u, ImVec2(size.x - 140 * u - ww, 140 * u), P(kBlue), which, true);
    }
    if (!trophies_.loaded) {
      const std::string start = std::string(T("START ", "INICIE O ")) + kGameTitles[trophy_game_] + T(" ONCE", " UMA VEZ");
      dl->AddText(g_item, 44 * u, ImVec2(140 * u + page_dx_, 220 * u), P(kWhite), start.c_str());
      dl->AddText(g_body, 28 * u, ImVec2(140 * u + page_dx_, 290 * u), P(kGrey),
                  T("The trophy list comes from the game itself: start it once, and the next time the menu opens "
                    "your trophies are here, with the ones you've earned.",
                    "A lista de troféus vem do próprio jogo: inicie-o uma vez, e da próxima vez que o menu abrir "
                    "seus troféus estarão aqui, com os que você já conquistou."),
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
    int counts[3] = {};
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
        const char* const kGroupNames[] = {T("EARNED", "CONQUISTADOS"), T("NOT EARNED YET", "AINDA NÃO CONQUISTADOS"),
                                           T("ONLINE ONLY", "SÓ ONLINE")};
        const std::string head = std::string(kGroupNames[rows[r].group]) + "   " + std::to_string(counts[rows[r].group]);
        dl->AddText(g_body, 21 * u, ImVec2(x, ry + 30 * u), P(kBlue), head.c_str());
        continue;
      }
      const Item& it = items_[size_t(rows[r].item)];
      const bool sel = rows[r].item == trophy_, earned = Earned(it);
      if (sel) dl->AddRectFilled(ImVec2(x - 16 * u, ry + 4 * u), ImVec2(right + 10 * u, ry + row_h - 4 * u),
                                 P(IM_COL32(70, 165, 255, 40)), 6 * u);
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
    const std::string of = std::to_string(trophies_.earned) + T(" of ", " de ") + std::to_string(trophies_.earnable) +
                           T(" earned", " conquistados");
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
                                 P(trophies_.Platinum() ? trophies::GradeColor(trophies::Grade::kPlatinum) : kBlue));
    py += 54 * u;

    const Item& it = items_[size_t(trophy_)];
    const float da = page_alpha_ * detail_in_;
    const float dx = px + (1 - detail_in_) * 14 * u;
    Icon(dl, it, dx, py, 112 * u, da);
    const trophies::Grade g = GradeOf(it);
    static constexpr const char* kGradesPt[] = {"BRONZE", "PRATA", "OURO", "PLATINA"};  // (trophies::Grade's order)
    const std::string grade = Portuguese() ? std::string("TROFÉU DE ") + kGradesPt[int(g) & 3]
                                           : Upper(std::string(trophies::GradeName(g)) + " trophy");
    dl->AddText(g_body, 20 * u, ImVec2(dx + 136 * u, py + 8 * u), trophies::GradeColor(g, da), grade.c_str());
    dl->AddText(g_item, 36 * u, ImVec2(dx + 136 * u, py + 34 * u), A(kWhite, da), NameOf(it).c_str(), nullptr,
                pw - 136 * u);
    py += 138 * u;
    const std::string text = TextOf(it);
    dl->AddText(g_body, 27 * u, ImVec2(dx, py), A(kText, da), text.c_str(), nullptr, pw);
    py += g_body->CalcTextSizeA(27 * u, FLT_MAX, pw, text.c_str()).y + 34 * u;
    const trophies::Trophy* t = it.index < 0 ? nullptr : &trophies_.trophies[size_t(it.index)];
    std::string status;
    if (Earned(it))
      status = T("Earned ", "Conquistado em ") +
               Date(t ? t->earned : trophies_.last_earned, T("%b %d, %Y", "%d de %b de %Y"));
    else if (!t) status = std::to_string(trophies_.earnable - trophies_.earned) + T(" to go", " restantes");
    else if (t->online()) status = T("Not available offline", "Indisponível offline");
    else status = T("Not earned yet", "Ainda não conquistado");
    dl->AddText(g_body, 20 * u, ImVec2(dx, py), A(kDim, da), "STATUS");
    dl->AddText(g_body, 24 * u, ImVec2(dx + 170 * u, py - 2 * u), A(kWhite, da), status.c_str());
    if (t) {
      py += 40 * u;
      dl->AddText(g_body, 20 * u, ImVec2(dx, py), A(kDim, da), "GAMERSCORE");
      dl->AddText(g_body, 24 * u, ImVec2(dx + 170 * u, py - 2 * u), A(kWhite, da),
                  (std::to_string(t->gamerscore) + "G").c_str());
    }
  }

  void DrawCredits(ImDrawList* dl, const ImVec2& size) {
    const float u = u_, x = 140 * u + page_dx_, w = 860 * u;
    float y = 180 * u;
    dl->PushClipRect(ImVec2(0, 0), ImVec2(size.x, size.y - 120 * u), true);  // (not into the clock)
    auto entry = [&](const char* name, const char* text) {
      dl->AddText(g_item, 26 * u, ImVec2(x, y), P(kWhite), name);
      y += 34 * u;
      dl->AddText(g_body, 20 * u, ImVec2(x, y), P(kGrey), text, nullptr, w);
      y += g_body->CalcTextSizeA(20 * u, FLT_MAX, w, text).y + 20 * u;
    };
    // the project
    dl->AddText(g_title, 60 * u, ImVec2(x, y - 6 * u), P(kWhite), "CRACKDOWN");
    const float tw = Width(g_title, 60 * u, "CRACKDOWN");
    Spaced(dl, g_item, 30 * u, ImVec2(x + tw + 22 * u, y + 24 * u), P(kBlue), "REACTIVATED", 6 * u);
    y += 84 * u;
    entry(T("BY CHAOSTHEORY1075", "POR CHAOSTHEORY1075"), "github.com/ChaosTheory1075/crackdown-ps5");
    dl->AddText(g_body, 20 * u, ImVec2(x, y), P(kBlue), T("ALL THE CREDIT GOES TO", "TODO O CRÉDITO VAI PARA"));
    y += 40 * u;
    entry("GTA IV FOR PS5  -  ELRIPALDA",
          T("The PS5 port this one is built on, top to bottom: the PS5 runtime, graphics and sound, the hub this menu "
            "copies, the trophies and their pop-up, and the installer payload. github.com/elripalda/gta4-ps5",
            "O port de PS5 em que este se baseia, de cima a baixo: o runtime do PS5, gráficos e som, o hub que este "
            "menu copia, os troféus e o aviso deles, e o payload do instalador. github.com/elripalda/gta4-ps5"));
    entry("CRACKDOWN RECOMPILED  -  SAULOB",
          T("The PC recompilation of Crackdown: the game fixes, the 60 FPS frame pacing, the cheats and the "
            "configuration this port runs. github.com/saulob/crackdown-xbox360-recompiled",
            "A recompilação de Crackdown para PC: as correções do jogo, o ritmo de 60 FPS, as trapaças e a "
            "configuração que este port roda. github.com/saulob/crackdown-xbox360-recompiled"));
    entry("CRACKDOWN 2 RECOMP  -  PACIFIC CITY RECOMPILED  /  UNOWA001",
          T("The PC recompilation of Crackdown 2: its 60 FPS game clock, which this port's Crackdown 2 runs. "
            "Copyright (c) 2026, Jerrod (Unowa001), BSD 3-Clause License. "
            "github.com/pacific-city-recompiled/crackdown-2-recomp",
            "A recompila\u00e7\u00e3o de Crackdown 2 para PC: o rel\u00f3gio de 60 FPS dele, que o Crackdown 2 deste port "
            "roda. Copyright (c) 2026, Jerrod (Unowa001), Licen\u00e7a BSD 3-Clause. "
            "github.com/pacific-city-recompiled/crackdown-2-recomp"));
    entry("LIBERTY RECOMPILED  -  OZORDI  /  VADUUR",
          T("OZORDI's recompilation of GTA IV and vaduur's LibertyRecompAndroid, which GTA IV for PS5 starts from.",
            "A recompilação de GTA IV do OZORDI e o LibertyRecompAndroid do vaduur, de onde GTA IV for PS5 parte."));
    entry(T("REXGLUE AND XENIA", "REXGLUE E XENIA"),
          T("The static recompiler and runtime under all of it, and the Xenia team's Xbox 360 emulator it grew from "
            "(Xenia Canary's XMA decoding is what plays the game's sound here).",
            "O recompilador estático e o runtime por baixo de tudo, e o emulador de Xbox 360 da equipe do Xenia de "
            "onde ele nasceu (a decodificação XMA do Xenia Canary é o que toca o som do jogo aqui)."));
    entry("MAX PAYNE 3 REPRESCRIBED  /  SAINTS ROW: REINSTATED  /  METAL GEAR RISING REPLATINUM",
          T("The sister ports: the same menu, installer, saves and trophies, FSR 1 and SMAA, and their fixes.",
            "Os ports irmãos: o mesmo menu, instalador, saves e troféus, FSR 1 e SMAA, e as correções deles."));

    const float px = 1060 * u + page_dx_, pw = size.x - 140 * u - px;
    float py = 180 * u;
    // the shout-out, on a panel of its own
    {
      const char* thanks =
          T("For making Crackdown: a unique, amazing game that will always hold up. Pacific City is still the best "
            "playground there is.",
            "Por criar Crackdown: um jogo único e incrível que sempre vai se manter. Pacific City ainda é o melhor "
            "parquinho que existe.");
      const float th = g_body->CalcTextSizeA(22 * u, FLT_MAX, pw - 48 * u, thanks).y;
      dl->AddRectFilled(ImVec2(px - 24 * u, py - 16 * u), ImVec2(px + pw, py + 96 * u + th),
                        P(IM_COL32(8, 8, 8, 170)), 6 * u);
      dl->AddRectFilled(ImVec2(px - 24 * u, py - 16 * u), ImVec2(px - 18 * u, py + 96 * u + th), P(kBlue));
      dl->AddText(g_body, 20 * u, ImVec2(px, py), P(kBlue), T("SPECIAL THANKS", "AGRADECIMENTOS ESPECIAIS"));
      dl->AddText(g_item, 40 * u, ImVec2(px, py + 30 * u), P(kWhite), "REALTIME WORLDS");
      dl->AddText(g_body, 22 * u, ImVec2(px, py + 84 * u), P(kGrey), thanks, nullptr, pw - 48 * u);
      py += 96 * u + th + 46 * u;
    }
    if (!song_.empty()) {
      dl->AddText(g_body, 20 * u, ImVec2(px, py), P(kBlue), T("MENU SONG", "MÚSICA DO MENU"));
      dl->AddText(g_body, 24 * u, ImVec2(px, py + 32 * u), P(kGrey), Fit(g_body, 24 * u, song_, pw).c_str());
      py += 90 * u;
    }
    dl->AddText(g_body, 20 * u, ImVec2(px, py), P(kBlue), T("PLEASE NOTE", "OBSERVAÇÃO"));
    py += 36 * u;
    const char* note =
        T("An unofficial fan project, not affiliated with or endorsed by Microsoft, Realtime Worlds or Sony. "
          "Crackdown and all related names belong to their owners. No game files are included: you need your own "
          "copy of Crackdown for the Xbox 360.",
          "Um projeto de fã não oficial, sem vínculo ou aprovação da Microsoft, Realtime Worlds ou Sony. Crackdown e "
          "todos os nomes relacionados pertencem aos seus donos. Nenhum arquivo do jogo está incluído: você precisa "
          "da sua própria cópia de Crackdown para Xbox 360.");
    dl->AddText(g_body, 20 * u, ImVec2(px, py), P(kGrey), note, nullptr, pw);
    py += g_body->CalcTextSizeA(20 * u, FLT_MAX, pw, note).y + 28 * u;
    dl->AddText(g_body, 20 * u, ImVec2(px, py), P(kBlue), T("BUILT WITH", "FEITO COM"));
    py += 36 * u;
    dl->AddText(g_body, 19 * u, ImVec2(px, py), P(kGrey),
                T("PS5 Payload SDK  /  Mesa RADV (Vulkan)  /  Dear ImGui\nstb_image, dr_mp3, zlib, xxHash  /  Oswald and "
                  "Inter (SIL Open Font License)\nAMD FidelityFX FSR 1, SMAA\nBlackBearReloaded's PS5 AT9 Converter "
                  "(the home screen music)",
                  "PS5 Payload SDK  /  Mesa RADV (Vulkan)  /  Dear ImGui\nstb_image, dr_mp3, zlib, xxHash  /  Oswald e "
                  "Inter (SIL Open Font License)\nAMD FidelityFX FSR 1, SMAA\nPS5 AT9 Converter do BlackBearReloaded "
                  "(a música da tela inicial)"),
                nullptr, pw);
    dl->PopClipRect();
  }

  // ---- files ----

  // The game's song (Crackdown's hub/music/, Crackdown 2's hub/music2/), in place of the one playing.
  void StartMusic(int game) {
    const fs::path dir = app_ / "hub" / (game == kCd3 ? "music3" : game == 2 ? "music2" : "music");
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      if (!e.is_regular_file() || !gta4ps5::sound::CanOpen(e.path())) continue;
      if (!music_) {
        if (!gta4ps5::sound::StartOutput()) {
          REXLOG_WARN("Crackdown menu: no audio port for the menu's song");
          return;
        }
        music_ = gta4ps5::sound::NewVoice();
      }
      if (auto src = gta4ps5::sound::Open(e.path())) {
        music_->Stop();
        music_->SetVolume(sets_[1].music / 100.0f);
        music_->Play(std::move(src), true);
        song_ = e.path().stem().string();
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
  fs::path app_, user_data_[4];  // (each game's UserData folder; 3: Crackdown 3's <app>/cd3)
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
  Settings sets_[3];      // each game's settings (1, 2)
  int game_ = 1;          // the game HOME has chosen
  int set_game_ = 1;      // SETTINGS: whose settings are shown
  int trophy_game_ = 1;   // TROPHIES: whose list is shown
  float shown_game_ = 1;  // (the background's crossfade)
  std::string start_error_;  // (cd2.bin couldn't be started)
  trophies::List lists_[4];
  cd3::Setup cd3_;        // (HOME's Crackdown 3: Prospero Win and its setup found)
  cd3::Job cd3_job_;      // (its install or sign-in, while the payload runs)
  std::string cd3_error_; // (a job that couldn't start)
  std::vector<bool> cd3_qr_;
  int cd3_qr_size_ = 0;
  std::string cd3_qr_text_;
  float cd3_tick_ = 0;
  bool in_list_ = false;  // SETTINGS: in the game's settings (false: on the game, at the left)
  float reset_t_ = -100;  // (when SQUARE last put the recommended settings back)
  Pad pad_;
  Image background_[4], logo_, title_;  // (each game's background; title_: hub/title.png, the header's name)
  gta4ps5::sound::Voice* music_ = nullptr;
  std::string song_;  // (the menu song's name, for the credits)
  Page page_ = kHome;
  int row_ = kFrameRate, trophy_ = 0;
  float settings_scroll_ = 0;  // (the settings list's scroll, eased)
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

void ApplySettings(const fs::path& app) {
#if defined(CD_GAME) && CD_GAME == 2
  // Crackdown 2's own set (menu2.ini, which its quick menu writes too), menu.ini only without one
  std::error_code ec;
  const bool own = fs::exists(app / "menu2.ini", ec);
  const Settings s = own ? Settings::Load(app, "menu2.ini") : Settings::Load(app);
  REXLOG_INFO("Crackdown 2: settings from {}, frame rate {} ({})", own ? "menu2.ini" : "menu.ini", s.fps2,
              s.fps2_chosen ? "the user's pick" : "the default");
  s.Apply();
#else
  Settings::Load(app).Apply();
#endif
}
ImFont* MenuItemFont() { return g_item; }

void ShowMenu(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, const fs::path& app,
              std::function<void()> play) {
  REXLOG_INFO("Crackdown menu: shown");
  new MenuDialog(drawer, immediate, app, std::move(play));  // (the dialog owns itself)
}

}  // namespace cdps5

#endif  // REX_PLATFORM_PS5
