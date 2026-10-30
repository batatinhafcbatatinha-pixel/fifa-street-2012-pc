// Native replacements for hot translated game functions, with the translated code's results.
//
// sub_82AA60E8 samples an animation pose between two key frames into the output matrices (~6% of
// the game/render thread in a match). Per bone: scale, rotation and translation, each either the
// bone's static value (20-byte descriptor) or the next half-float group of key streams A and B;
// lerp by t, rotation as nlerp along the shorter arc (one Newton-Raphson step on 1/sqrt), quaternion
// to rows, rows scaled, w column masked, translation with w = 1. Then the root translation, the
// parent chain (row-vector products, children after parents) and an optional uniform scale of the
// translations. The vector work below is done with the same operations, in the same order and in
// the same flush-to-zero mode as the translated code, so the output matrices are identical; other
// paths (no descriptor, the alternate sampler at obj+16) run the translated code.
//
// STREET4_NO_NATIVE_ANIM=1   translated code only
// STREET4_NATIVE_CHECK=1     run native and translated on the same input and compare the outputs
//                            ("[native-check] anim MISMATCH" / periodic totals)
// STREET4_NATIVE_AB=1        alternate native/translated every 600 presented frames and log
//                            "[native-ab]" with the frame time of each block (also covers the
//                            renderer's byte-swapped constant copy, see renderer.cpp)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/intrinsics.h>

REX_EXTERN(sub_82738530);  // the game's memcpy

namespace street4 {
namespace natives {
namespace {
bool EnvSet(const char* name) {
  const char* v = std::getenv(name);
  return v && *v && *v != '0';
}
const bool g_anim_enabled = !EnvSet("STREET4_NO_NATIVE_ANIM");
const bool g_check = EnvSet("STREET4_NATIVE_CHECK");
const bool g_ab = EnvSet("STREET4_NATIVE_AB");
std::atomic<bool> g_ab_on{true};
std::atomic<uint64_t> g_anim_calls{0}, g_anim_mismatches{0};
}  // namespace

bool NativeOn() { return !g_ab || g_ab_on.load(std::memory_order_relaxed); }

namespace {
// CPU time of the calling thread in ms.
double ThreadCpuMs() {
#ifdef _WIN32
  FILETIME c, e, k, u;
  if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
  auto ms = [](FILETIME f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e4; };
  return ms(k) + ms(u);
#else
  timespec ts{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
#endif
}
}  // namespace

// Called once per presented frame (renderer.cpp).
void FrameTick() {
  if (!g_ab) return;
  static uint64_t frames = 0;
  static auto start = std::chrono::steady_clock::now();
  static double cpu_start = ThreadCpuMs();
  if (++frames % 600 != 0) return;
  auto now = std::chrono::steady_clock::now();
  double ms = std::chrono::duration<double, std::milli>(now - start).count();
  double cpu = ThreadCpuMs();
  start = now;
  bool was = g_ab_on.load(std::memory_order_relaxed);
  REXLOG_INFO("[native-ab] native={} frames=600 ms={:.1f} fps={:.2f} thread_cpu_ms/frame={:.3f}", was ? 1 : 0, ms,
              600000.0 / ms, (cpu - cpu_start) / 600.0);
  cpu_start = cpu;
  g_ab_on.store(!was, std::memory_order_relaxed);
}
}  // namespace natives
}  // namespace street4

namespace {
using street4::natives::g_anim_calls;
using street4::natives::g_anim_enabled;
using street4::natives::g_anim_mismatches;
using street4::natives::g_check;

// Vectors are kept the way the translated code keeps VMX registers: the 16 big-endian bytes
// reversed, so host lane 3 is element 0 (x) and lane 0 is element 3 (w).
using V = simde__m128i;
using F = simde__m128;

uint8_t* Host(uint8_t* base, uint32_t a) { return base + a + (a >= 0xE0000000u ? 0x1000u : 0u); }
uint32_t Load32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Host(base, a), 4);
  return __builtin_bswap32(v);
}
uint16_t Load16(uint8_t* base, uint32_t a) {
  uint16_t v;
  std::memcpy(&v, Host(base, a), 2);
  return __builtin_bswap16(v);
}
void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(Host(base, a), &v, 4);
}
V Rev(V v) { return simde_mm_shuffle_epi8(v, simde_mm_loadu_si128(reinterpret_cast<const V*>(rex::ppc::VectorMaskL))); }
// 16 bytes at any address (lvlx + lvrx pair).
V LoadU(uint8_t* base, uint32_t a) { return Rev(simde_mm_loadu_si128(reinterpret_cast<const V*>(Host(base, a)))); }
// lvx / stvx.
V LoadA(uint8_t* base, uint32_t a) { return Rev(simde_mm_load_si128(reinterpret_cast<const V*>(Host(base, a & ~0xFu)))); }
void StoreA(uint8_t* base, uint32_t a, V v) { simde_mm_store_si128(reinterpret_cast<V*>(Host(base, a & ~0xFu)), Rev(v)); }

F AsF(V v) { return simde_mm_castsi128_ps(v); }
V AsV(F f) { return simde_mm_castps_si128(f); }
// vsel: bits of b where the mask is set, else a.
V Sel(V a, V b, V mask) { return simde_mm_or_si128(simde_mm_andnot_si128(mask, a), simde_mm_and_si128(mask, b)); }
// vsldoi v,v,v,8: swap the 8-byte halves.
V Swap64(V v) { return simde_mm_alignr_epi8(v, v, 8); }
F Splat(F v, int element) {  // vspltw
  V i = AsV(v);
  switch (element) {
    case 0: return AsF(simde_mm_shuffle_epi32(i, 0xFF));
    case 1: return AsF(simde_mm_shuffle_epi32(i, 0xAA));
    case 2: return AsF(simde_mm_shuffle_epi32(i, 0x55));
    default: return AsF(simde_mm_shuffle_epi32(i, 0x00));
  }
}
F MAdd(F a, F b, F c) { return simde_mm_add_ps(simde_mm_mul_ps(a, b), c); }  // vmaddfp (not fused)
F NMSub(F a, F b, F c) {                                                         // vnmsubfp
  return simde_mm_xor_ps(simde_mm_sub_ps(simde_mm_mul_ps(a, b), c), simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000))));
}
// vupkd3d128 type 20: four halves (elements 4-7 of the register) to floats; no Inf/NaN, and
// denormal halves become signed zero.
F Half4(V v) {
  const V h = simde_mm_cvtepu16_epi32(v);
  const V sign = simde_mm_slli_epi32(simde_mm_and_si128(h, simde_mm_set1_epi32(0x8000)), 16);
  const V exponent = simde_mm_and_si128(h, simde_mm_set1_epi32(0x7C00));
  const V mantissa = simde_mm_and_si128(h, simde_mm_set1_epi32(0x03FF));
  const V normal = simde_mm_or_si128(
      sign, simde_mm_or_si128(simde_mm_slli_epi32(simde_mm_add_epi32(exponent, simde_mm_set1_epi32(0x1C000)), 13),
                              simde_mm_slli_epi32(mantissa, 13)));
  return AsF(Sel(normal, sign, simde_mm_cmpeq_epi32(exponent, simde_mm_setzero_si128())));
}
// Half-float group at the start of 16 loaded bytes.
F Group(V v) { return Half4(Swap64(v)); }
// lfs then stfs (float -> double -> float through the FPU registers).
uint32_t FloatThroughDouble(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  volatile double d = double(f);
  f = float(d);
  std::memcpy(&bits, &f, 4);
  return bits;
}

constexpr uint32_t kMaskTable = 0x82382060;   // [0] = keep the static value, [7] = take the key
constexpr uint32_t kRotationScale = 0x82313580;
constexpr uint32_t kRowMask = 0x83D83960;     // clears w of the rotation rows
constexpr uint32_t kPermRow0 = 0x8236F680, kPermRow1 = 0x82365C40, kPermRow2 = 0x8236F690;
constexpr uint32_t kOneFloat = 0x82301EB4;

// Main path of sub_82AA60E8. Registers on entry: r3 sampler, r4/r5 key frames A/B, r7 output
// matrices, r9 animation (+44 descriptors), f1 t, f2 scale; r1 the caller's stack pointer.
void SamplePose(PPCContext& ctx, uint8_t* base) {
  const uint32_t sampler = ctx.r3.u32, key_a = ctx.r4.u32, key_b = ctx.r5.u32, out = ctx.r7.u32;
  const uint32_t descriptors = Load32(base, ctx.r9.u32 + 44);
  const uint32_t sp = ctx.r1.u32;

  ctx.fpscr.disableFlushMode();
  float t;
  {
    float f1 = float(ctx.f1.f64);
    uint32_t bits;
    std::memcpy(&bits, &f1, 4);
    Store32(base, sp + 44, bits);  // stfs f1,364(r1) of the 320-byte frame (caller's save area)
    std::memcpy(&t, &bits, 4);
  }
  const double scale = ctx.f2.f64;
  const uint32_t skeleton = Load32(base, sampler + 8);
  const uint32_t bind_pose = Load32(base, sampler + 4);
  const uint32_t skip_list = Load32(base, skeleton + 36);
  const uint32_t bones = Load32(base, skeleton + 8);
  const uint32_t matrices = Load32(base, skeleton + 12);
  const uint32_t channel_flags = Load32(base, skeleton + 20);
  const uint32_t parents = Load32(base, skeleton + 28);
  const uint32_t out_index = Load32(base, skeleton + 32);
  if (bind_pose) {  // memcpy(out, bind pose, matrices * 64)
    uint64_t saved_sp = ctx.r1.u64, saved_lr = ctx.lr;
    ctx.r1.u64 = uint32_t(sp - 320);
    ctx.r3.u64 = out;
    ctx.r4.u64 = bind_pose;
    ctx.r5.u64 = uint32_t(matrices << 6);
    ctx.lr = 0x82AA61EC;
    sub_82738530(ctx, base);
    ctx.r1.u64 = saved_sp;
    ctx.lr = saved_lr;  // restored by the epilogue
  }

  ctx.fpscr.disableFlushMode();
  uint32_t a[3], b[3];
  for (int i = 0; i < 3; ++i) {
    a[i] = FloatThroughDouble(Load32(base, key_a + 4 * i));
    b[i] = FloatThroughDouble(Load32(base, key_b + 4 * i));
  }
  const F root_a = AsF(simde_mm_set_epi32(int(a[0]), int(a[1]), int(a[2]), 0));
  const F root_b = AsF(simde_mm_set_epi32(int(b[0]), int(b[1]), int(b[2]), 0));
  const F tv = AsF(simde_mm_set1_epi32([&] { uint32_t u; std::memcpy(&u, &t, 4); return int(u); }()));

  ctx.fpscr.enableFlushModeUnconditional();
  const F one = simde_mm_cvtepi32_ps(simde_mm_set1_epi32(1));
  const F zero = AsF(simde_mm_set1_epi32(0));
  if (bones) {
    const F half = simde_mm_mul_ps(simde_mm_cvtepi32_ps(simde_mm_set1_epi32(1)), simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x3F000000))));
    const V take_static = LoadA(base, kMaskTable), take_key = LoadA(base, kMaskTable + 7 * 16);
    const F rotation_scale = AsF(LoadA(base, kRotationScale));
    const V row_mask = LoadA(base, kRowMask);
    const V perm0 = LoadA(base, kPermRow0), perm1 = LoadA(base, kPermRow1), perm2 = LoadA(base, kPermRow2);
    uint32_t pa = key_a + 14, pb = key_b + 14, pd = descriptors + 6;
    for (uint32_t bone = 0; bone < bones; ++bone, pd += 20) {
      const uint32_t flags = *Host(base, channel_flags + bone);
      const uint32_t o_scale = (flags & 4) ? 6 : 0, o_rot = (flags & 1) ? 8 : 0, o_trans = (flags & 2) ? 6 : 0;
      const V m_scale = (flags & 4) ? take_key : take_static;
      const V m_rot = (flags & 1) ? take_key : take_static;
      const V m_trans = (flags & 2) ? take_key : take_static;

      const F sa = Group(Sel(LoadU(base, pd - 6), LoadU(base, pa), m_scale));
      const F sb = Group(Sel(LoadU(base, pd - 6), LoadU(base, pb), m_scale));
      const F ra = Group(Sel(LoadU(base, pd), LoadU(base, pa + o_scale), m_rot));
      const F rb = Group(Sel(LoadU(base, pd), LoadU(base, pb + o_scale), m_rot));
      const F ta = Group(Sel(LoadU(base, pd + 8), LoadU(base, pa + o_scale + o_rot), m_trans));
      const F tb = Group(Sel(LoadU(base, pd + 8), LoadU(base, pb + o_scale + o_rot), m_trans));
      const uint32_t index = uint32_t(int32_t(int16_t(Load16(base, out_index + 2 * bone)))) << 6;
      pa += o_scale + o_rot + o_trans;
      pb += o_scale + o_rot + o_trans;

      const F s = MAdd(tv, simde_mm_sub_ps(sb, sa), sa);
      const F dot = simde_mm_dp_ps(ra, rb, 0xFF);
      const F r_near = MAdd(tv, simde_mm_sub_ps(rb, ra), ra);
      const F r_far = NMSub(simde_mm_add_ps(rb, ra), tv, ra);
      const F r = AsF(Sel(AsV(r_far), AsV(r_near), AsV(simde_mm_cmpgt_ps(dot, zero))));
      const F len2 = simde_mm_dp_ps(r, r, 0xFF);
      F rs = simde_mm_div_ps(simde_mm_set1_ps(1), simde_mm_sqrt_ps(len2));
      rs = MAdd(simde_mm_mul_ps(rs, half), NMSub(len2, simde_mm_mul_ps(rs, rs), one), rs);
      const F q = simde_mm_mul_ps(rotation_scale, simde_mm_mul_ps(r, rs));

      const F qw = AsF(simde_mm_shuffle_epi32(AsV(q), 0x00));
      const F p0 = simde_mm_mul_ps(q, AsF(simde_mm_shuffle_epi32(AsV(q), 0x9F)));
      const F p1 = simde_mm_mul_ps(qw, AsF(simde_mm_shuffle_epi32(AsV(q), 0x7B)));
      const F d = NMSub(q, q, half);
      const F sum = simde_mm_add_ps(p0, p1), diff = simde_mm_sub_ps(p0, p1);
      const F diag = simde_mm_add_ps(d, AsF(simde_mm_shuffle_epi32(AsV(d), 0x9F)));
      F row0 = AsF(rex::ppc::simde_mm_perm_epi8_(AsV(sum), AsV(diff), perm0));
      F row1 = AsF(rex::ppc::simde_mm_perm_epi8_(AsV(diff), AsV(diag), perm1));
      F row2 = AsF(rex::ppc::simde_mm_perm_epi8_(AsV(diag), AsV(sum), perm2));
      row0 = simde_mm_blend_ps(row0, simde_mm_permute_ps(diag, 78), 2);
      row1 = simde_mm_blend_ps(row1, simde_mm_permute_ps(sum, 57), 2);
      row2 = simde_mm_blend_ps(row2, simde_mm_permute_ps(diff, 228), 2);
      const F row2s = simde_mm_mul_ps(row0, Splat(s, 2));
      const F row1s = simde_mm_mul_ps(row1, Splat(s, 1));
      const F row0s = simde_mm_mul_ps(row2, Splat(s, 0));
      F trans = MAdd(simde_mm_sub_ps(tb, ta), tv, ta);
      trans = simde_mm_blend_ps(trans, simde_mm_permute_ps(one, 228), 1);

      const uint32_t dst = out + index;
      StoreA(base, dst + 32, simde_mm_and_si128(AsV(row2s), row_mask));
      StoreA(base, dst + 16, simde_mm_and_si128(AsV(row1s), row_mask));
      StoreA(base, dst, simde_mm_and_si128(AsV(row0s), row_mask));
      StoreA(base, dst + 48, AsV(trans));
    }
  }

  // Root translation: lerp of the key frames' translations, w = 1.
  ctx.fpscr.enableFlushMode();
  {
    const uint32_t root = out + (uint32_t(int32_t(int16_t(Load16(base, out_index + 2)))) << 6) + 48;
    F p = MAdd(simde_mm_sub_ps(root_b, root_a), tv, root_a);
    p = simde_mm_blend_ps(p, simde_mm_permute_ps(one, 228), 1);
    StoreA(base, root, AsV(p));
  }

  // Parent chain: child rows times the parent matrix.
  for (uint32_t j = 1; j < matrices; ++j) {
    if (skip_list && Load16(base, skip_list + 2 * j) == 0xFFFF) continue;
    const uint32_t c = out + 64 * j, p = out + (uint32_t(Load16(base, parents + 2 * j)) << 6);
    const F c0 = AsF(LoadA(base, c)), c1 = AsF(LoadA(base, c + 16)), c2 = AsF(LoadA(base, c + 32)), c3 = AsF(LoadA(base, c + 48));
    const F p0 = AsF(LoadA(base, p)), p1 = AsF(LoadA(base, p + 16)), p2 = AsF(LoadA(base, p + 32)), p3 = AsF(LoadA(base, p + 48));
    auto row = [&](F cr) {
      F v = simde_mm_mul_ps(p0, Splat(cr, 0));
      v = MAdd(p1, Splat(cr, 1), v);
      v = MAdd(p2, Splat(cr, 2), v);
      return MAdd(p3, Splat(cr, 3), v);
    };
    const F n0 = row(c0), n1 = row(c1), n2 = row(c2), n3 = row(c3);
    StoreA(base, c, AsV(n0));
    StoreA(base, c + 16, AsV(n1));
    StoreA(base, c + 32, AsV(n2));
    StoreA(base, c + 48, AsV(n3));
  }

  // Uniform scale of the translations unless the scale is 1.
  ctx.fpscr.disableFlushMode();
  const uint32_t one_bits = Load32(base, kOneFloat);
  float one_f;
  std::memcpy(&one_f, &one_bits, 4);
  const double one_d = double(one_f);
  if (!(scale == one_d)) {
    float sf = float(scale);
    uint32_t s_bits, o_bits = FloatThroughDouble(one_bits);
    std::memcpy(&s_bits, &sf, 4);
    const F sv = AsF(simde_mm_set_epi32(int(s_bits), int(s_bits), int(s_bits), int(o_bits)));
    ctx.fpscr.enableFlushMode();
    for (uint32_t i = 0; i < matrices; ++i) {
      const uint32_t a3 = out + 64 * i + 48;
      StoreA(base, a3, AsV(simde_mm_mul_ps(AsF(LoadA(base, a3)), sv)));
    }
  }
  ctx.fpscr.disableFlushMode();
  ctx.r3.u64 = 0;
}

// Highest matrix the main path writes, for the check mode's snapshot.
uint32_t WrittenMatrices(PPCContext& ctx, uint8_t* base) {
  const uint32_t skel = Load32(base, ctx.r3.u32 + 8);
  const uint32_t bones = Load32(base, skel + 8), matrices = Load32(base, skel + 12), out_index = Load32(base, skel + 32);
  uint32_t top = matrices;
  for (uint32_t i = 0; i < bones; ++i) {
    int32_t idx = int16_t(Load16(base, out_index + 2 * i));
    if (idx >= 0) top = std::max<uint32_t>(top, uint32_t(idx) + 1);
  }
  int32_t root = int16_t(Load16(base, out_index + 2));
  if (root >= 0) top = std::max<uint32_t>(top, uint32_t(root) + 1);
  return top;
}
}  // namespace

REX_EXTERN(__imp__sub_82AA60E8);
REX_HOOK_RAW(sub_82AA60E8) {
  const bool main_path = Load32(base, ctx.r9.u32 + 44) != 0 &&
                         !(*Host(base, ctx.r3.u32 + 16) != 0 && (ctx.r10.u32 & 0xFF) != 0);
  if (!g_anim_enabled || !main_path || !street4::natives::NativeOn()) {
    __imp__sub_82AA60E8(ctx, base);
    return;
  }
  if (!g_check) {
    SamplePose(ctx, base);
    return;
  }
  // Check: native first, then the translated code on the same input; compare the matrices.
  const uint32_t out = ctx.r7.u32, sp = ctx.r1.u32;
  const uint32_t bytes = WrittenMatrices(ctx, base) * 64;
  std::vector<uint8_t> before(Host(base, out), Host(base, out) + bytes);
  const uint32_t save_before = Load32(base, sp + 44);
  auto entry = std::make_unique<PPCContext>(ctx);  // off the (fiber) stack
  SamplePose(ctx, base);
  std::vector<uint8_t> native(Host(base, out), Host(base, out) + bytes);
  const uint32_t save_native = Load32(base, sp + 44);
  const uint64_t r3_native = ctx.r3.u64;
  std::memcpy(Host(base, out), before.data(), bytes);
  Store32(base, sp + 44, save_before);
  ctx = *entry;
  ctx.fpscr.restoreGuestBits(entry->fpscr.csr);  // MXCSR back in step with the cached csr
  __imp__sub_82AA60E8(ctx, base);
  const uint64_t calls = g_anim_calls.fetch_add(1) + 1;
  bool same = std::memcmp(Host(base, out), native.data(), bytes) == 0 && Load32(base, sp + 44) == save_native &&
              ctx.r3.u64 == r3_native;
  if (!same) {
    uint64_t m = g_anim_mismatches.fetch_add(1) + 1;
    if (m <= 20) {
      uint32_t first = 0;
      while (first < bytes && Host(base, out)[first] == native[first]) ++first;
      REXLOG_WARN("[native-check] anim MISMATCH out={:08X} bytes={} first differing byte {} (matrix {} row {})", out,
                  bytes, first, first / 64, (first % 64) / 16);
    }
  }
  if ((calls & 0x3FFF) == 0)
    REXLOG_INFO("[native-check] anim calls={} mismatches={}", calls, g_anim_mismatches.load());
}
