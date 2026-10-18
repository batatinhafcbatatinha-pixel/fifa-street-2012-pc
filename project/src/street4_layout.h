// Relocates the launcher's (default.xex) indirect-call dispatch table.
//
// The SDK places a module's table at image_base + image_size. For default.xex that is
// 0x821C0000-0x82412E90, which overlaps fifadllzf.xex.dll (fixed base 0x82300000), so
// the DLL load overwrote the launcher's table and the SDK refused to wire the DLL. The
// launcher's table goes to 0x87000000 instead (free, above the DLL and its table): the app
// reports image_base/size accordingly (fifastreet4_app.h), and this header, force-included
// after the generated PCH, points the launcher's recompiled lookups there.
#pragma once

#define STREET4_EXE_TABLE_BASE 0x87000000ull
#define STREET4_EXE_LAYOUT_IMAGE_BASE 0x86F00000u
#define STREET4_EXE_LAYOUT_IMAGE_SIZE 0x00100000u

#ifdef REX_LOOKUP_FUNC
#undef REX_LOOKUP_FUNC
#define REX_LOOKUP_FUNC(x, y) \
  (*(PPCFunc**)(x + STREET4_EXE_TABLE_BASE + (u64(u32(y) - REX_CODE_BASE) * 2)))
#endif
