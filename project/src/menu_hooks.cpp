// The game's own menu leads to the PC settings (compiled into the game module, fifadllzf).
// The main menu's corner line "NOT CONNECTED TO EA SERVERS" (BACK) runs a flow that asks for
// the Xbox LIVE sign-in UI, a console service the PC does not have: that request opens the PC
// settings instead, and the line is renamed where the game copies its localized text
// (std::string::assign from a character range; the game has one copy of it per allocator).
// When the PC settings close, the pad reports one press of A, which cancels the flow's
// "connecting" box the way the player would. The "Xbox LIVE" line (the online modes) reads
// "ONLINE". fifastreet4-pc.ini [opcoes] menu_no_jogo=0 restores the original texts.
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>

#include "guest_device.h"
#include "pc_settings.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Rename {
  const char* from;  // as stored by the game (UTF-8)
  const char* to;    // at most as long, in bytes
};

// The game's languages (STREET4_MENU_TEXTS=1 logs the texts of the line's call site).
const Rename kRenames[] = {
    {"NOT CONNECTED TO EA SERVERS", "PC SETTINGS"},
    {"NON CONNECT\xC3\x89 AUX SERVEURS EA", "PARAM\xC3\x88TRES PC"},
    {"NICHT MIT EA-SERVERN VERBUNDEN", "PC-EINSTELLUNGEN"},
    {"NON CONNESSO AI SERVER EA", "IMPOSTAZIONI PC"},
    {"NO CONECTADO A LOS SERVIDORES DE EA", "CONFIGURACI\xC3\x93N DEL PC"},
    // Brand name, the same in the five languages.
    {"Xbox LIVE", "ONLINE"},
};

// Return address of the EA servers flow's sign-in request (set from the first logged run).
constexpr uint32_t kFlowSignIn = 0x82779F5C;

std::atomic<int> g_entry{0};
std::atomic<int64_t> g_requested{0};
std::atomic<int64_t> g_press_until{0};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

void RenameRange(uint8_t* base, uint32_t begin, uint32_t end, uint32_t lr) {
  const uint32_t length = end - begin;
  if (!begin || length < 8 || length > 64 || !pcs::GameMenuEntry()) return;
  char* text = reinterpret_cast<char*>(street4::GuestPtr(base, begin));
  if (lr == 0x832C576Cu && std::getenv("STREET4_MENU_TEXTS")) {
    static std::mutex mu;
    static std::set<std::string> seen;
    std::lock_guard<std::mutex> lock(mu);
    if (seen.insert(std::string(text, length)).second)
      REXLOG_INFO("[menu] text via 832C576C: {}", std::string(text, length));
  }
  for (const Rename& r : kRenames) {
    size_t n = std::strlen(r.from);
    if (n == length && std::memcmp(text, r.from, n) == 0) {
      size_t m = std::strlen(r.to);
      std::memcpy(text, r.to, m);
      std::memset(text + m, ' ', n - m);
      REXLOG_INFO("[menu] renamed \"{}\" (assign from {:08X})", r.from, lr);
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

REX_EXTERN(__imp__sub_82773738);
REX_EXTERN(__imp__sub_835730E8);

// sub_82773738(this, user, online_only): the platform layer's "show the sign-in UI"
// (XShowSigninUI); 0 = shown. The EA servers flow (sub_82779898) calls it when the player asks.
REX_HOOK_RAW(sub_82773738) {
  REXLOG_INFO("[menu] sign-in UI requested (user {}) from {:08X}", ctx.r4.u32, uint32_t(ctx.lr));
  if (!pcs::GameMenuEntry() || uint32_t(ctx.lr) != kFlowSignIn) {
    __imp__sub_82773738(ctx, base);
    return;
  }
  REXLOG_INFO("[menu] EA servers line chosen: opening the PC settings");
  g_requested = NowMs();
  g_entry = 1;
  pcs::RequestMenu();
  ctx.r3.u64 = 0;
}

// sub_835730E8(user, state): XInputGetState. state+4 is the buttons (big-endian), A = 0x1000.
REX_HOOK_RAW(sub_835730E8) {
  const uint32_t state = ctx.r4.u32;
  __imp__sub_835730E8(ctx, base);
  // The game does not read the pad while the menu holds it, so the first read after the
  // request (once the menu is no longer open) is the moment it closed.
  if (g_entry.load() && !pcs::MenuOpen() && NowMs() - g_requested.load() > 250) {
    g_entry = 0;
    g_press_until = NowMs() + 150;
    REXLOG_INFO("[menu] PC settings closed: pressing A to cancel the EA servers box");
  }
  if (ctx.r3.u32 == 0 && state && NowMs() < g_press_until.load()) {
    uint8_t* buttons = street4::GuestPtr(base, state + 4);
    buttons[0] |= 0x10;  // A (0x1000), high byte first
  }
}
