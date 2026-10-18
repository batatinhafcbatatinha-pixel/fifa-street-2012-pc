// Hooks on the XDK Direct3D library inside fifadllzf.xex.dll (compiled into the game
// module target so they replace the generated weak symbols for direct and indirect calls).
//
// The D3D library still runs: it keeps its state shadows, builds its command segments in
// memory nobody reads, and does its bookkeeping. These hooks add what the GPU used to do:
//  - draw / resolve / present through the native D3D11 renderer;
//  - publish timestamps immediately (rendering is synchronous);
//  - deliver the CP interrupt callbacks it requests (frame flips, fences).
// Addresses and ABIs: NATIVE_PLAN.md and hle/d3d_functions.json.

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <fmt/format.h>
#include <string>

#include <algorithm>
#include <cstring>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "guest_device.h"
#include "renderer.h"
#include "street4_graphics.h"

using namespace street4;

namespace {

std::atomic<uint32_t> g_device{0};

// Diagnostics: per-hook call counters, logged every 2 s by a host thread.
struct Counters {
  std::atomic<uint64_t> draws{0}, inline_draws{0}, resolves{0}, presents{0}, clears{0},
      callbacks{0}, timestamps{0}, kicks{0}, tiling{0};
} g_counters;

void StartStatsThread() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::thread([] {
      while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        REXLOG_INFO("street4 stats: draws={} inline={} resolves={} presents={} clears={} "
                    "cp_callbacks={} timestamps={} kicks={} tiling={}",
                    g_counters.draws.load(), g_counters.inline_draws.load(),
                    g_counters.resolves.load(), g_counters.presents.load(),
                    g_counters.clears.load(), g_counters.callbacks.load(),
                    g_counters.timestamps.load(), g_counters.kicks.load(),
                    g_counters.tiling.load());
        // Last call return address of each guest thread (where it is waiting).
        auto* ks = rex::Runtime::instance()->kernel_state();
        std::string lrs;
        for (uint32_t id = 1; id < 256; ++id) {
          auto t = ks->GetThreadByID(id);
          if (!t || !t->thread_state() || !t->thread_state()->context()) continue;
          auto* c = t->thread_state()->context();
          lrs += fmt::format(" [{} lr={:08X} r3={:08X}]", id, c->lr, c->r3.u32);
        }
        REXLOG_INFO("street4 threads:{}", lrs);
        if (uint32_t d = g_device.load()) {
          uint8_t* b = rex::Runtime::instance()->memory()->virtual_membase();
          uint32_t blk = LoadBE32(GuestPtr(b, d + dev::kStatusBlock));
          REXLOG_INFO("street4 device {:08X}: 5FF8={:08X} 55A4={:08X} 5CC8={:08X} 5F38={:08X} 2B90={} ts={:08X} done={:08X} flags2B3C={:02X} 2B3D={:02X}",
                      d, LoadBE32(GuestPtr(b, d + 0x5FF8)), LoadBE32(GuestPtr(b, d + 0x55A4)), LoadBE32(GuestPtr(b, d + 0x5CC8)),
                      LoadBE32(GuestPtr(b, d + 0x5F38)), LoadBE32(GuestPtr(b, d + 0x2B90)),
                      LoadBE32(GuestPtr(b, d + dev::kTimestamp)), blk ? LoadBE32(GuestPtr(b, blk)) : 0,
                      *GuestPtr(b, d + 0x2B3C), *GuestPtr(b, d + 0x2B3D));
        }
      }
    }).detach();
  });
}

void StoreBE32(uint8_t* base, uint32_t address, uint32_t value) {
  uint32_t v = rex::byte_swap(value);
  std::memcpy(GuestPtr(base, address), &v, 4);
}

// Inline vertex data started with BeginVertices and finished by EndVertices (or by the
// DrawVerticesUP wrapper, which ends it inline).
struct PendingInline {
  bool active = false;
  uint32_t device = 0;
  DrawRequest draw;
} g_pending;

void FlushPending(uint8_t* base) {
  if (!g_pending.active) return;
  g_pending.active = false;
  GuestDevice dev(base, g_pending.device);
  Renderer::Get().Draw(dev, g_pending.draw);
}

}  // namespace

// ----------------------------------------------------------------------------- sync
REX_EXTERN(__imp__sub_82714060);
// Ends a command segment and asks the GPU to write the timestamp / ring position into the
// status block. Rendering is synchronous, so everything issued so far is complete: the
// completed timestamp published is the library's current one (fences are never ahead).
// Not while the library's command-buffer workers (async command buffer calls, tiling
// replay) still own submitted segments (device+0x2B90 > 0): completing early lets the
// library reuse segment memory the workers are still reading.

void PublishCompletion(uint8_t* base, uint32_t dev) {
  g_device = dev;
  uint32_t block = LoadBE32(GuestPtr(base, dev + dev::kStatusBlock));
  if (!block || LoadBE32(GuestPtr(base, dev + 0x2B90)) != 0) return;
  StoreBE32(base, block, LoadBE32(GuestPtr(base, dev + dev::kTimestamp)));
}

void PollCompletion(void*) {
  uint32_t dev = g_device;
  if (!dev) return;
  uint8_t* base = rex::Runtime::instance()->memory()->virtual_membase();
  PublishCompletion(base, dev);
}

void RegisterPoller() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (auto* gfx = NativeGraphicsSystem::Get()) gfx->SetCompletionPoller(PollCompletion, nullptr);
  });
}

REX_HOOK_RAW(sub_82714060) {
  StartStatsThread();
  RegisterPoller();
  ++g_counters.timestamps;
  uint32_t dev = ctx.r3.u32;
  uint32_t position = (LoadBE32(GuestPtr(base, dev + 0x3BCC)) & 3) | LoadBE32(GuestPtr(base, dev + dev::kRingWrite));
  __imp__sub_82714060(ctx, base);
  uint32_t block = LoadBE32(GuestPtr(base, dev + dev::kStatusBlock));
  if (block) StoreBE32(base, block + 4, position);
  PublishCompletion(base, dev);
}

REX_EXTERN(__imp__sub_82713710);
// BlockOnFence(device, fence, ...): already complete (see PublishCompletion).
REX_HOOK_RAW(sub_82713710) {
  PublishCompletion(base, ctx.r3.u32);
  static std::atomic<int> logged{0};
  if (logged++ < 20) {
    uint32_t dev = ctx.r3.u32, block = LoadBE32(GuestPtr(base, dev + dev::kStatusBlock));
    REXLOG_INFO("street4 BlockOnFence dev={:08X} fence={:08X} ts={:08X} status_block={:08X} done={:08X} flags={:02X}",
                dev, ctx.r4.u32, LoadBE32(GuestPtr(base, dev + dev::kTimestamp)), block,
                block ? LoadBE32(GuestPtr(base, block)) : 0, *GuestPtr(base, dev + 0x2B3D));
  }
  __imp__sub_82713710(ctx, base);
}

REX_EXTERN(__imp__sub_827141A0);
// Requests "run function(argument) in the CP interrupt once the GPU gets here".
REX_HOOK_RAW(sub_827141A0) {
  StartStatsThread();
  ++g_counters.callbacks;
  uint32_t dev = ctx.r3.u32, function = ctx.r6.u32, argument = ctx.r7.u32;
  __imp__sub_827141A0(ctx, base);
  if (function) {
    uint32_t block = LoadBE32(GuestPtr(base, dev + dev::kCallbackBlock));
    if (auto* gfx = NativeGraphicsSystem::Get()) gfx->QueueGpuCallback(block, function, argument);
  }
}

REX_EXTERN(__imp__sub_82713B80);
// Kicks command segments to the primary ring (writes CP_RB_WPTR into the RAM sink).
REX_HOOK_RAW(sub_82713B80) {
  StartStatsThread();
  ++g_counters.kicks;
  uint32_t dev = ctx.r3.u32;
  __imp__sub_82713B80(ctx, base);
  if (auto* gfx = NativeGraphicsSystem::Get()) gfx->ReportReadPointer(LoadBE32(GuestPtr(base, dev + 0x2B50)));
}

REX_EXTERN(__imp__sub_82713368);
REX_HOOK_RAW(sub_82713368) {
  uint32_t write_index = ctx.r4.u32;
  __imp__sub_82713368(ctx, base);
  if (auto* gfx = NativeGraphicsSystem::Get()) gfx->ReportReadPointer(write_index);
}

// ----------------------------------------------------------------------------- draws
REX_EXTERN(__imp__sub_8271C8C8);
// DrawIndexedVertices(device, primitive, base_vertex, start_index, index_count).
REX_HOOK_RAW(sub_8271C8C8) {
  StartStatsThread();
  ++g_counters.draws;
  FlushPending(base);
  uint32_t dev = ctx.r3.u32;
  DrawRequest d;
  d.primitive = ctx.r4.u32;
  d.base_vertex = int32_t(ctx.r5.u32);
  uint32_t start = ctx.r6.u32;
  d.count = ctx.r7.u32;
  d.indexed = true;
  __imp__sub_8271C8C8(ctx, base);
  uint32_t ib = LoadBE32(GuestPtr(base, dev + dev::kIndexBuffer));
  if (!ValidGuest(ib) || !ValidGuest(LoadBE32(GuestPtr(base, ib + 0x18)))) return;
  d.index32 = (LoadBE32(GuestPtr(base, ib)) >> 31) & 1;
  d.index_address = LoadBE32(GuestPtr(base, ib + 0x18)) + start * (d.index32 ? 4 : 2);
  Renderer::Get().Draw(GuestDevice(base, dev), d);
}

REX_EXTERN(__imp__sub_8271C4B0);
// DrawVertices(device, primitive, start_vertex, vertex_count).
REX_HOOK_RAW(sub_8271C4B0) {
  StartStatsThread();
  ++g_counters.draws;
  FlushPending(base);
  uint32_t dev = ctx.r3.u32;
  DrawRequest d;
  d.primitive = ctx.r4.u32;
  d.first_vertex = 0;
  d.base_vertex = 0;
  uint32_t start = ctx.r5.u32;
  d.count = ctx.r6.u32;
  __imp__sub_8271C4B0(ctx, base);
  d.first_vertex = start;
  Renderer::Get().Draw(GuestDevice(base, dev), d);
}

REX_EXTERN(__imp__sub_8271B9C8);
// BeginVertices(device, primitive, vertex_count, stride) -> pointer the caller fills.
// The library binds the data to vertex fetch constant 95 (group 31, dwords 4-5).
REX_HOOK_RAW(sub_8271B9C8) {
  StartStatsThread();
  ++g_counters.inline_draws;
  FlushPending(base);
  uint32_t dev = ctx.r3.u32, primitive = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  __imp__sub_8271B9C8(ctx, base);
  uint32_t data = ctx.r3.u32;
  if (!ValidGuest(data)) return;
  g_pending.active = true;
  g_pending.device = dev;
  g_pending.draw = DrawRequest{};
  g_pending.draw.primitive = primitive;
  g_pending.draw.count = count;
  g_pending.draw.vfetch_override = true;
  g_pending.draw.override_index = 95;
  g_pending.draw.override_address = data;
  g_pending.draw.override_bytes = count * stride;
  g_pending.draw.override_endian = 2;
}

REX_EXTERN(__imp__sub_8271B9B8);
// EndVertices(device).
REX_HOOK_RAW(sub_8271B9B8) {
  __imp__sub_8271B9B8(ctx, base);
  FlushPending(base);
}

REX_EXTERN(__imp__sub_8271BE88);
// DrawVerticesUP(device, primitive, vertex_count, data, stride): Begin + copy + inline End.
REX_HOOK_RAW(sub_8271BE88) {
  __imp__sub_8271BE88(ctx, base);
  FlushPending(base);
}

REX_EXTERN(__imp__sub_8271C410);
// DrawIndexedVerticesUP(device, primitive, min_index, vertex_count, index_count, indices,
//                       index_format, vertex_data, stride[stack]).
REX_HOOK_RAW(sub_8271C410) {
  StartStatsThread();
  ++g_counters.inline_draws;
  FlushPending(base);
  uint32_t dev = ctx.r3.u32;
  DrawRequest d;
  d.primitive = ctx.r4.u32;
  uint32_t vertex_count = ctx.r6.u32 + ctx.r5.u32;
  d.count = ctx.r7.u32;
  d.index_address = ctx.r8.u32;
  d.index32 = (ctx.r9.u32 & 4) != 0;
  uint32_t vertex_data = ctx.r10.u32;
  uint32_t stride = LoadBE32(GuestPtr(base, ctx.r1.u32 + 0x54));
  d.indexed = true;
  __imp__sub_8271C410(ctx, base);
  d.vfetch_override = true;
  d.override_index = 95;
  d.override_address = vertex_data;
  d.override_bytes = vertex_count * stride;
  d.override_endian = 2;
  Renderer::Get().Draw(GuestDevice(base, dev), d);
}

// ----------------------------------------------------------------------------- resolve
REX_EXTERN(__imp__sub_82713018);
// Resolve(device, flags, source_rect*, dest_texture*, dest_level, dest_point*, slice,
//         clear_color*, clear_z(f1), clear_stencil[stack], parameters*[stack]).
REX_HOOK_RAW(sub_82713018) {
  StartStatsThread();
  ++g_counters.resolves;
  FlushPending(base);
  uint32_t dev = ctx.r3.u32;
  ResolveRequest r;
  r.flags = ctx.r4.u32;
  if (uint32_t rect = ctx.r5.u32; ValidGuest(rect)) {
    r.src_x0 = int32_t(LoadBE32(GuestPtr(base, rect)));
    r.src_y0 = int32_t(LoadBE32(GuestPtr(base, rect + 4)));
    r.src_x1 = int32_t(LoadBE32(GuestPtr(base, rect + 8)));
    r.src_y1 = int32_t(LoadBE32(GuestPtr(base, rect + 12)));
  }
  r.has_dest = ctx.r6.u32 != 0;
  if (uint32_t point = ctx.r8.u32; ValidGuest(point)) {
    r.dst_x = int32_t(LoadBE32(GuestPtr(base, point)));
    r.dst_y = int32_t(LoadBE32(GuestPtr(base, point + 4)));
  }
  __imp__sub_82713018(ctx, base);
  Renderer::Get().Resolve(GuestDevice(base, dev), r);
}

// ----------------------------------------------------------------------------- present
REX_EXTERN(__imp__sub_8270FDC0);
// Present/Swap(device, front_buffer_texture*, ...). The texture's fetch constant is at +0x1C.
REX_HOOK_RAW(sub_8270FDC0) {
  StartStatsThread();
  ++g_counters.presents;
  FlushPending(base);
  uint32_t front = ctx.r4.u32;
  uint8_t fetch[24] = {};
  if (!ValidGuest(front)) front = 0;
  if (front) std::memcpy(fetch, GuestPtr(base, front + 0x1C), 24);
  __imp__sub_8270FDC0(ctx, base);
  FlushPending(base);
  if (front) Renderer::Get().Present(fetch);
}

// ----------------------------------------------------------------------------- clears
REX_EXTERN(__imp__sub_8270ED88);
// Clear(device, rect_count, rects*, flags, color(ARGB), z(f1), stencil).
// flags: bits 0-3 render targets, 0x10 depth, 0x20 stencil.
REX_HOOK_RAW(sub_8270ED88) {
  StartStatsThread();
  ++g_counters.clears;
  FlushPending(base);
  uint32_t dev = ctx.r3.u32, count = ctx.r4.u32, rects = ctx.r5.u32, flags = ctx.r6.u32;
  uint32_t color = ctx.r7.u32, stencil = ctx.r9.u32;
  float z = float(ctx.f1.f64);
  __imp__sub_8270ED88(ctx, base);
  float rgba[4] = {float((color >> 16) & 0xFF) / 255.0f, float((color >> 8) & 0xFF) / 255.0f,
                   float(color & 0xFF) / 255.0f, float(color >> 24) / 255.0f};
  std::vector<int32_t> r;
  if (ValidGuest(rects))
    for (uint32_t i = 0; i < std::min<uint32_t>(count, 64) * 4; ++i)
      r.push_back(int32_t(LoadBE32(GuestPtr(base, rects + i * 4))));
  Renderer::Get().ClearBound(GuestDevice(base, dev), flags & 0xF, rgba, (flags & 0x10) != 0, z,
                             (flags & 0x20) != 0, uint8_t(stencil), r.data(),
                             uint32_t(r.size() / 4));
}

REX_EXTERN(__imp__sub_82710C68);
// BeginTiling(device, flags, tile_count, tile_rects*, clear_color(float4*), clear_z(f1),
//             clear_stencil). Tiles map to one full-size host surface, so clear it once.
REX_HOOK_RAW(sub_82710C68) {
  StartStatsThread();
  ++g_counters.tiling;
  FlushPending(base);
  uint32_t dev = ctx.r3.u32, color = ctx.r7.u32, stencil = ctx.r9.u32;
  float z = float(ctx.f1.f64);
  __imp__sub_82710C68(ctx, base);
  if (!ValidGuest(color)) return;
  float rgba[4];
  for (int i = 0; i < 4; ++i) {
    uint32_t v = LoadBE32(GuestPtr(base, color + i * 4));
    std::memcpy(&rgba[i], &v, 4);
  }
  Renderer::Get().ClearBound(GuestDevice(base, dev), 1, rgba, true, z, true, uint8_t(stencil));
}

// ----------------------------------------------------------------------------- queries
REX_EXTERN(__imp__sub_82703B40);
// Occlusion query End(query): asks the GPU to write depth sample counts (8 dwords per
// slot, 0x20 bytes, at query+0x34; slot count at query+0x1C) that the engine then polls
// for (it waits while the 0xFFFFFEED sentinel is still there). Published immediately as
// "visible": Total/ZPass = 0x00101000 (same value in either byte order), fails = 0.
REX_HOOK_RAW(sub_82703B40) {
  FlushPending(base);
  uint32_t query = ctx.r3.u32;
  uint32_t results = ValidGuest(query) ? LoadBE32(GuestPtr(base, query + 0x34)) : 0;
  uint32_t slots = ValidGuest(query) ? std::max<uint32_t>(LoadBE32(GuestPtr(base, query + 0x1C)), 1) : 0;
  __imp__sub_82703B40(ctx, base);
  if (!ValidGuest(results)) return;
  constexpr uint32_t kVisible = 0x00101000;
  for (uint32_t i = 0; i < std::min<uint32_t>(slots, 16); ++i) {
    uint32_t r = results + i * 0x20;
    const uint32_t counts[8] = {kVisible, kVisible, 0, 0, kVisible, kVisible, 0, 0};
    for (int k = 0; k < 8; ++k) StoreBE32(base, r + k * 4, counts[k]);
  }
}

// ----------------------------------------------------------------------------- diagnostics
REX_EXTERN(__imp__sub_827145B0);
// Segment kick helper: logs callers that make device+0x2B90 grow (pending worker items).
REX_HOOK_RAW(sub_827145B0) {
  uint32_t dev = ctx.r3.u32, lr = ctx.lr, r7 = ctx.r7.u32, r9 = ctx.r9.u32;
  uint32_t before = LoadBE32(GuestPtr(base, dev + 0x2B90));
  __imp__sub_827145B0(ctx, base);
  uint32_t after = LoadBE32(GuestPtr(base, dev + 0x2B90));
  static std::atomic<int> logged{0};
  if (after != before && logged++ < 30)
    REXLOG_INFO("street4 2B90 {} -> {} by 827145B0 lr={:08X} r7={} r9={}", before, after, lr, r7, r9);
}

REX_EXTERN(__imp__sub_826FCC30);
// D3D wait-loop step (hang detector). Logs which loops spin for long.
REX_HOOK_RAW(sub_826FCC30) {
  uint32_t lr = ctx.lr;
  __imp__sub_826FCC30(ctx, base);
  static std::atomic<uint64_t> spins{0};
  static std::atomic<uint32_t> last_lr{0};
  uint64_t n = ++spins;
  if (last_lr.exchange(lr) != lr || (n % 200000) == 0)
    if (n < 50 || (n % 200000) == 0) REXLOG_INFO("street4 spin #{} lr={:08X}", n, lr);
}

REX_EXTERN(__imp__sub_827160B0);
REX_HOOK_RAW(sub_827160B0) {
  uint32_t lr = ctx.lr;
  __imp__sub_827160B0(ctx, base);
  static std::atomic<int> logged{0};
  if (logged++ < 20) REXLOG_INFO("street4 GPU hang recovery lr={:08X} -> {}", lr, ctx.r3.u32);
}
