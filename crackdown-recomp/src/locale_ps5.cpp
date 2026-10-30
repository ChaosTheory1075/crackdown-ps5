// The game's language, as the menu's LANGUAGE picks it (Title Update 6 addresses).
//
// At start-up the title picks one of its text folders (game/Local/<locale>) at 0x823567EC:
// the console's locale first (XGetLocale: the Czech Republic, Hungary, Poland and Slovakia have
// their own text), then its language (XGetLanguage; Spanish is es-LA when XGetGameRegion is 0xFF),
// and hands the folder's number to sub_823C1188. The PS5 has no Xbox 360 locale to give it, and
// the runtime always reported a US console, so the Central European texts and es-LA were out of
// reach. Here sub_823C1188 is given crackdown_locale instead, when it is set:
//
//   0 en-US   1 fr-FR   2 it-IT   3 de-DE   4 es-ES   5 es-LA   6 cs-CZ
//   7 pl-PL   8 hu-HU   9 ja-JP  10 ko-KR  11 zh-TW  13 sk-SK
//
// Title Update 6 adds no languages: its update/Local has these same 13 folders (the DLC's text).

#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_INT32(crackdown_locale, -1, "Crackdown",
                     "The game's text folder (-1: the title's own choice; 0 en-US ... 13 sk-SK)");

REX_EXTERN(__imp__sub_823C1188);

REX_HOOK_RAW(sub_823C1188) {
  const int32_t locale = REXCVAR_GET(crackdown_locale);
  REXLOG_INFO("Crackdown: the title picked text folder {}, using {}", ctx.r3.u32,
              locale >= 0 ? uint32_t(locale) : ctx.r3.u32);
  if (locale >= 0 && locale <= 13 && locale != 12) ctx.r3.u64 = uint32_t(locale);
  __imp__sub_823C1188(ctx, base);
}
