// Diagnostics: forwards the game's CRT printf output (0x82737EA8) to the log.
// Integer/string conversions only; the original printf still runs.

#include <cstdio>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>

#include "guest_device.h"

namespace {

std::string FormatGuest(uint8_t* base, uint32_t fmt, const uint64_t* args, int arg_count) {
  std::string out;
  const char* f = reinterpret_cast<const char*>(street4::GuestPtr(base, fmt));
  int next = 0;
  for (int guard = 0; *f && guard < 4096; ++f, ++guard) {
    if (*f != '%') {
      out += *f;
      continue;
    }
    std::string spec = "%";
    ++f;
    while (*f && std::string("-+ #0123456789.lhzjtL").find(*f) != std::string::npos) {
      if (*f != 'l' && *f != 'h' && *f != 'z' && *f != 'j' && *f != 't' && *f != 'L') spec += *f;
      ++f;
    }
    if (!*f) break;
    char c = *f;
    if (c == '%') {
      out += '%';
      continue;
    }
    uint64_t v = next < arg_count ? args[next] : 0;
    ++next;
    char buf[512];
    switch (c) {
      case 's': {
        uint32_t p = uint32_t(v);
        std::string s = p ? std::string(reinterpret_cast<const char*>(street4::GuestPtr(base, p))).substr(0, 400) : "(null)";
        snprintf(buf, sizeof(buf), (spec + "s").c_str(), s.c_str());
        break;
      }
      case 'd': case 'i':
        snprintf(buf, sizeof(buf), (spec + "d").c_str(), int32_t(v));
        break;
      case 'u': case 'x': case 'X': case 'o': case 'c':
        snprintf(buf, sizeof(buf), (spec + c).c_str(), uint32_t(v));
        break;
      case 'p':
        snprintf(buf, sizeof(buf), "%08X", uint32_t(v));
        break;
      default:
        snprintf(buf, sizeof(buf), "<%c>", c);
    }
    out += buf;
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

}  // namespace

REX_EXTERN(__imp__sub_82737EA8);
REX_HOOK_RAW(sub_82737EA8) {
  uint64_t args[7] = {ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.r7.u64,
                      ctx.r8.u64, ctx.r9.u64, ctx.r10.u64};
  if (ctx.r3.u32) REXLOG_INFO("[game] {}", FormatGuest(base, ctx.r3.u32, args, 7));
  __imp__sub_82737EA8(ctx, base);
}

// Kernel DbgPrint used by the game's debug channels.
REX_HOOK_RAW(__imp__DbgPrint) {
  uint64_t args[7] = {ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.r7.u64,
                      ctx.r8.u64, ctx.r9.u64, ctx.r10.u64};
  if (ctx.r3.u32) REXLOG_INFO("[game-dbg] {}", FormatGuest(base, ctx.r3.u32, args, 7));
  ctx.r3.u64 = 0;
}

// EA fatal/assert message formatter (followed by an unconditional trap at its call sites).
REX_EXTERN(__imp__sub_826C6148);
REX_HOOK_RAW(sub_826C6148) {
  uint64_t args[7] = {ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.r7.u64,
                      ctx.r8.u64, ctx.r9.u64, ctx.r10.u64};
  if (ctx.r3.u32)
    REXLOG_ERROR("[game-assert] {} (lr={:08X})", FormatGuest(base, ctx.r3.u32, args, 7),
                 ctx.lr);
  __imp__sub_826C6148(ctx, base);
}
