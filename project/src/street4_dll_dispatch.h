// Appended to the game module's precompiled header (CMakeLists.txt): indirect calls.
// A call through a null pointer is skipped (logged once per process) instead of
// terminating; it happens in the D3D library's command-buffer worker threads, whose
// callbacks only prepared data for the GPU command processor.
#pragma once

#include <atomic>

#include <rex/logging.h>

#ifdef REX_CALL_INDIRECT_FUNC
#undef REX_CALL_INDIRECT_FUNC
#define REX_CALL_INDIRECT_FUNC(x)                                                       \
  do {                                                                                  \
    REX_PROFILE_INDIRECT_DISPATCH();                                                    \
    uint32_t rex_indirect_target_ = (uint32_t)(x);                                      \
    PPCFunc* rex_indirect_func_;                                                        \
    if ((uint32_t)(rex_indirect_target_ - REX_CODE_BASE) <                              \
        REX_CODE_SIZE + REX_THUNK_RESERVE_SIZE) [[likely]] {                            \
      rex_indirect_func_ = REX_LOOKUP_FUNC(base, rex_indirect_target_);                 \
    } else {                                                                            \
      rex_indirect_func_ = nullptr;                                                     \
    }                                                                                   \
    if (!rex_indirect_func_) [[unlikely]] {                                             \
      ctx.last_indirect_target = rex_indirect_target_;                                  \
      if (rex_indirect_target_ == 0) {                                                  \
        static std::atomic<bool> rex_null_logged_{false};                               \
        if (!rex_null_logged_.exchange(true))                                           \
          REXLOG_WARN("street4: null indirect call skipped at lr={:08X}", ctx.lr);      \
        break;                                                                          \
      }                                                                                 \
      rex_indirect_func_ = rex::runtime::ResolveIndirectFunction(rex_indirect_target_); \
    }                                                                                   \
    rex_indirect_func_(ctx, base);                                                      \
  } while (0)
#endif
