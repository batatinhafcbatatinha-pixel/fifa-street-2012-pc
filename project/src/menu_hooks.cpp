// The game's own menus on PC (compiled into the game module, fifadllzf): the main menu's
// "Xbox LIVE" line (the online modes) reads "ONLINE". The game copies every localized text it
// shows with std::string::assign from a character range (one copy of it per allocator); the
// range is rewritten there, shorter texts padded with spaces so the copy's length stays right.
// fifastreet4-pc.ini [opcoes] menu_no_jogo=0 keeps the original.
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>

#include "guest_device.h"
#include "pc_settings.h"

namespace {

struct Rename {
  const char* from;  // as stored by the game (UTF-8)
  const char* to;    // at most as long, in bytes
};

// Brand names, the same in the game's five languages.
const Rename kRenames[] = {
    {"Xbox LIVE", "ONLINE"},
};

void RenameRange(uint8_t* base, uint32_t begin, uint32_t end, uint32_t lr) {
  const uint32_t length = end - begin;
  if (!begin || length < 4 || length > 64) return;
  char* text = reinterpret_cast<char*>(street4::GuestPtr(base, begin));
  // STREET4_MENU_TEXTS=1 logs each distinct text and its call site (to find more lines).
  if (std::getenv("STREET4_MENU_TEXTS")) {
    static std::mutex mu;
    static std::set<std::string> seen;
    std::lock_guard<std::mutex> lock(mu);
    if (seen.insert(std::string(text, length)).second)
      REXLOG_INFO("[menu] text via {:08X}: {}", lr, std::string(text, length));
  }
  if (!pcs::GameMenuEntry()) return;
  for (const Rename& r : kRenames) {
    size_t n = std::strlen(r.from);
    if (n == length && std::memcmp(text, r.from, n) == 0) {
      size_t m = std::strlen(r.to);
      std::memcpy(text, r.to, m);
      std::memset(text + m, ' ', n - m);
      return;
    }
  }
}

}  // namespace

#define STRING_ASSIGN_HOOK(fn)                                   \
  REX_EXTERN(__imp__##fn);                                       \
  REX_HOOK_RAW(fn) {                                             \
    RenameRange(base, ctx.r4.u32, ctx.r5.u32, uint32_t(ctx.lr)); \
    __imp__##fn(ctx, base);                                      \
  }

// std::string::assign(string, begin, end), one per allocator.
STRING_ASSIGN_HOOK(sub_826441E8)
STRING_ASSIGN_HOOK(sub_829047F8)
STRING_ASSIGN_HOOK(sub_827B00A0)
STRING_ASSIGN_HOOK(sub_8325A338)
STRING_ASSIGN_HOOK(sub_833BD7C0)
