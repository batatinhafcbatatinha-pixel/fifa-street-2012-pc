#pragma once
#include <bit>
#include <cstdint>
#include "../pc_settings.h"

// Engine-independent policy. Copa's port should reuse the constant rules after
// identifying its shader hashes, not copy Street 4's guest addresses.
namespace ea_postfx {
enum Effect { Vignette, BrightnessContrast, Count };
enum class Pass { Other, Vignette, BrightnessContrast };
inline Pass Identify(uint64_t pixel_shader_hash) {
  switch (pixel_shader_hash) {
    case 0xba02458e2167f262ull: return Pass::Vignette;
    case 0xa392657905ecd8c0ull: return Pass::BrightnessContrast;
    default: return Pass::Other;
  }
}
inline bool Enabled(Pass pass) {
  switch (pass) {
    case Pass::Vignette: return pcs::EffectEnabled(Vignette);
    case Pass::BrightnessContrast: return pcs::EffectEnabled(BrightnessContrast);
    default: return true;
  }
}
// Host-endian PS float constants, after literal overlay. Keep the original draw:
// it copies the current scene into its output; skipping it would leave stale data.
inline void Neutralize(Pass pass, uint32_t* ps) {
  if (pass == Pass::Vignette) {
    // c1.w mixes scene RGB towards vignette colour. Zero = unchanged scene.
    ps[1 * 4 + 3] = 0;
  } else if (pass == Pass::BrightnessContrast) {
    // Output = scene * c0.y + c0.x. Identity preserves the alpha/depth contract.
    ps[0] = 0;
    ps[1] = std::bit_cast<uint32_t>(1.0f);
  }
}
}
