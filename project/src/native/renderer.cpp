// FIFA Street 4 native renderer (Direct3D 11). See renderer.h.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/window.h>

#include "renderer.h"
#include "shader_translator.h"
#include "texture_loader.h"

namespace street4 {

namespace {

template <typename T>
struct Com {
  T* p = nullptr;
  Com() = default;
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;
  Com(Com&& o) noexcept : p(o.p) { o.p = nullptr; }
  Com& operator=(Com&& o) noexcept {
    if (this != &o) {
      reset();
      p = o.p;
      o.p = nullptr;
    }
    return *this;
  }
  ~Com() { reset(); }
  void reset() {
    if (p) p->Release();
    p = nullptr;
  }
  T** put() {
    reset();
    return &p;
  }
  T* get() const { return p; }
  T* operator->() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

uint64_t Hash64(const void* data, size_t size, uint64_t seed = 0x9E3779B97F4A7C15ull) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint64_t h = seed ^ (size * 0xC2B2AE3D27D4EB4Full);
  size_t i = 0;
  for (; i + 8 <= size; i += 8) {
    uint64_t v;
    std::memcpy(&v, p + i, 8);
    v *= 0x87C37B91114253D5ull;
    v = (v << 31) | (v >> 33);
    h ^= v * 0x4CF5AD432745937Full;
    h = ((h << 27) | (h >> 37)) * 5 + 0x52DCE729;
  }
  for (; i < size; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
  h ^= h >> 33;
  h *= 0xFF51AFD7ED558CCDull;
  h ^= h >> 33;
  return h;
}

float AsFloat(uint32_t v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

uint32_t VirtualToPhysical(uint32_t address) {
  // GPU view of the CPU physical mappings (0xA0000000/0xC0000000/0xE0000000).
  return (address & 0x1FFFFFFF) + (((address >> 20) + 0x200) & 0x1000);
}

// --------------------------------------------------------------------------- formats
DXGI_FORMAT ColorRtFormat(uint32_t f) {
  switch (f) {
    case 0: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 1: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case 2: case 10: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 3: case 12: return DXGI_FORMAT_R16G16B16A16_FLOAT;  // 7e3 float
    case 4: case 6: return DXGI_FORMAT_R16G16_FLOAT;
    case 5: case 7: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 14: return DXGI_FORMAT_R32_FLOAT;
    case 15: return DXGI_FORMAT_R32G32_FLOAT;
    default: return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
}

// Host format a resolve destination (xenos::ColorFormat, numbered like TextureFormat) uses.
DXGI_FORMAT ResolveDestFormat(uint32_t color_format) {
  switch (color_format) {
    case 2: case 8: case 9: return DXGI_FORMAT_R8_UNORM;
    case 4: return DXGI_FORMAT_B5G6R5_UNORM;
    case 3: return DXGI_FORMAT_B5G5R5A1_UNORM;
    case 6: case 14: case 50: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 7: case 54: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 10: return DXGI_FORMAT_R8G8_UNORM;
    case 15: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 16: case 17: case 55: case 56: return DXGI_FORMAT_R11G11B10_FLOAT;
    case 24: return DXGI_FORMAT_R16_UNORM;
    case 25: return DXGI_FORMAT_R16G16_UNORM;
    case 26: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case 30: return DXGI_FORMAT_R16_FLOAT;
    case 31: return DXGI_FORMAT_R16G16_FLOAT;
    case 32: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 36: return DXGI_FORMAT_R32_FLOAT;
    case 37: return DXGI_FORMAT_R32G32_FLOAT;
    case 38: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
}

D3D11_COMPARISON_FUNC CompareFunc(uint32_t f) {
  static const D3D11_COMPARISON_FUNC t[8] = {
      D3D11_COMPARISON_NEVER,   D3D11_COMPARISON_LESS,      D3D11_COMPARISON_EQUAL,
      D3D11_COMPARISON_LESS_EQUAL, D3D11_COMPARISON_GREATER, D3D11_COMPARISON_NOT_EQUAL,
      D3D11_COMPARISON_GREATER_EQUAL, D3D11_COMPARISON_ALWAYS};
  return t[f & 7];
}
D3D11_STENCIL_OP StencilOp(uint32_t o) {
  static const D3D11_STENCIL_OP t[8] = {
      D3D11_STENCIL_OP_KEEP,     D3D11_STENCIL_OP_ZERO,     D3D11_STENCIL_OP_REPLACE,
      D3D11_STENCIL_OP_INCR_SAT, D3D11_STENCIL_OP_DECR_SAT, D3D11_STENCIL_OP_INVERT,
      D3D11_STENCIL_OP_INCR,     D3D11_STENCIL_OP_DECR};
  return t[o & 7];
}
D3D11_BLEND BlendFactor(uint32_t f, bool alpha) {
  switch (f) {
    case 0: return D3D11_BLEND_ZERO;
    case 1: return D3D11_BLEND_ONE;
    case 4: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 5: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 6: return D3D11_BLEND_SRC_ALPHA;
    case 7: return D3D11_BLEND_INV_SRC_ALPHA;
    case 8: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 9: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 10: return D3D11_BLEND_DEST_ALPHA;
    case 11: return D3D11_BLEND_INV_DEST_ALPHA;
    case 12: case 14: return D3D11_BLEND_BLEND_FACTOR;
    case 13: case 15: return D3D11_BLEND_INV_BLEND_FACTOR;
    case 16: return D3D11_BLEND_SRC_ALPHA_SAT;
    default: return D3D11_BLEND_ONE;
  }
}
D3D11_BLEND_OP BlendOp(uint32_t o) {
  switch (o) {
    case 1: return D3D11_BLEND_OP_SUBTRACT;
    case 2: return D3D11_BLEND_OP_MIN;
    case 3: return D3D11_BLEND_OP_MAX;
    case 4: return D3D11_BLEND_OP_REV_SUBTRACT;
    default: return D3D11_BLEND_OP_ADD;
  }
}
D3D11_TEXTURE_ADDRESS_MODE AddressMode(uint32_t m) {
  switch (m) {
    case 0: return D3D11_TEXTURE_ADDRESS_WRAP;
    case 1: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 2: case 4: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 3: case 5: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    default: return D3D11_TEXTURE_ADDRESS_BORDER;
  }
}

// --------------------------------------------------------------------------- shaders
using D3DCompileFn = decltype(&D3DCompile);

const char* kRectListGs = R"HLSL(
struct V { float4 position : SV_Position; float4 interp[16] : TEXCOORD0; float4 misc : TEXCOORD16; };
V Lerp3(V a, V b, V c) {  // b + c - a
  V r;
  r.position = b.position + c.position - a.position;
  [unroll] for (uint i = 0; i < 16; ++i) r.interp[i] = b.interp[i] + c.interp[i] - a.interp[i];
  r.misc = b.misc + c.misc - a.misc;
  return r;
}
[maxvertexcount(4)]
void main(triangle V v[3], inout TriangleStream<V> s) {
  float2 p0 = v[0].position.xy / v[0].position.w, p1 = v[1].position.xy / v[1].position.w,
         p2 = v[2].position.xy / v[2].position.w;
  float d01 = dot(p1 - p0, p1 - p0), d12 = dot(p2 - p1, p2 - p1), d20 = dot(p0 - p2, p0 - p2);
  uint corner = (d12 >= d01 && d12 >= d20) ? 0 : (d20 >= d01 ? 1 : 2);
  V c = v[corner], a = v[(corner + 1) % 3], b = v[(corner + 2) % 3];
  s.Append(a); s.Append(c); s.Append(b); s.Append(Lerp3(c, a, b));
  s.RestartStrip();
}
)HLSL";

const char* kBlitHlsl = R"HLSL(
Texture2D<float4> src : register(t0);
SamplerState samp : register(s0);
cbuffer Blit : register(b0) { float4 src_rect; float4 params; };  // uv origin/size, scale
struct O { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
O vs_main(uint id : SV_VertexID) {
  O o;
  float2 t = float2((id << 1) & 2, id & 2);
  o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
  o.uv = src_rect.xy + t * src_rect.zw;
  return o;
}
float4 ps_main(O i) : SV_Target { return src.SampleLevel(samp, i.uv, 0) * params.x; }
float ps_depth(O i) : SV_Target { return src.SampleLevel(samp, i.uv, 0).r; }
)HLSL";

struct ShaderEntry {
  TranslatedShader meta;
  Com<ID3D11VertexShader> vs;
  Com<ID3D11PixelShader> ps;
  bool failed = false;
};

struct RenderTarget {
  uint32_t edram_base = 0, format = 0, pitch = 0;
  bool depth = false;
  uint32_t width = 0, height = 0;
  Com<ID3D11Texture2D> texture;
  Com<ID3D11RenderTargetView> rtv;
  Com<ID3D11DepthStencilView> dsv;
  Com<ID3D11ShaderResourceView> srv;
  uint64_t last_used = 0;
};

struct HostTexture {
  TextureDesc desc;
  Com<ID3D11Texture2D> texture;
  Com<ID3D11ShaderResourceView> srv2d, srv_array, srv_cube;
  uint64_t sample_hash = 0;
  uint64_t validated_frame = ~0ull;
  uint64_t last_used = 0;
  bool cube = false;
  uint32_t array_size = 1;
};

struct ResolvedTexture {
  uint32_t address = 0, width = 0, height = 0, format = 0;
  Com<ID3D11Texture2D> texture;
  Com<ID3D11RenderTargetView> rtv;
  Com<ID3D11ShaderResourceView> srv;
  uint64_t guest_hash = 0;
  uint64_t frame = 0;
};

struct VertexBufferEntry {
  Com<ID3D11Buffer> buffer;
  Com<ID3D11ShaderResourceView> srv;
  uint32_t capacity = 0;
  uint64_t hash = 0;
  uint64_t validated_frame = ~0ull;
  uint64_t last_used = 0;
};

class RendererImpl {
 public:
  bool Ready() { return EnsureDevice(); }

  void Draw(const GuestDevice& dev, const DrawRequest& draw);
  void Resolve(const GuestDevice& dev, const ResolveRequest& req);
  void ClearBound(const GuestDevice& dev, uint32_t color_targets, const float rgba[4],
                  bool depth, float z, bool stencil, uint8_t s, const int32_t* rects,
                  uint32_t rect_count);
  void Present(const uint8_t* fetch);
  uint64_t frame() const { return frame_; }

 private:
  bool EnsureDevice();
  bool EnsureSwapChain();
  uint8_t* Virtual(uint32_t address) { return GuestPtr(membase_, address); }
  const uint8_t* Physical(uint32_t address) { return physbase_ + (address & 0x1FFFFFFF); }

  ShaderEntry* GetShader(ShaderStage stage, const ShaderMicrocode& code, const ShaderKey& key);
  bool Compile(const std::string& source, const char* entry, const char* target,
               std::vector<uint8_t>& out, const std::string& cache_name);
  RenderTarget* GetRenderTarget(bool depth, uint32_t base, uint32_t format, uint32_t pitch,
                                uint32_t height);
  ID3D11ShaderResourceView* GetTexture(const uint32_t fetch[6], TextureDimension dim,
                                       float size_out[4]);
  ID3D11SamplerState* GetSampler(const uint32_t fetch[6]);
  ID3D11ShaderResourceView* GetVertexBuffer(uint32_t physical, uint32_t bytes, bool cacheable,
                                            const uint8_t* host);
  bool BuildIndices(const DrawRequest& draw, D3D11_PRIMITIVE_TOPOLOGY& topology,
                    std::vector<uint32_t>& indices, bool& use_indices, bool& rect_list,
                    const GuestDevice& dev);
  void SetupStates(const GuestDevice& dev, uint32_t color_mask_active, bool depth_active);
  void UpdateDynamicBuffer(Com<ID3D11Buffer>& buffer, uint32_t& capacity, UINT bind,
                           const void* data, uint32_t bytes);
  ResolvedTexture* GetResolved(uint32_t address, uint32_t width, uint32_t height,
                               uint32_t format, bool create);
  void BlitTo(ID3D11RenderTargetView* rtv, uint32_t dst_w, uint32_t dst_h, D3D11_VIEWPORT vp,
              ID3D11ShaderResourceView* src, float src_w, float src_h, float u0, float v0,
              float u1, float v1, bool depth);

  // Device.
  Com<ID3D11Device> device_;
  Com<ID3D11DeviceContext> context_;
  Com<ID3D11DeviceContext1> context1_;
  Com<IDXGISwapChain1> swap_chain_;
  Com<ID3D11RenderTargetView> backbuffer_rtv_;
  HWND hwnd_ = nullptr;
  uint32_t bb_width_ = 0, bb_height_ = 0;
  bool device_failed_ = false;
  D3DCompileFn compile_ = nullptr;
  std::filesystem::path cache_dir_;
  uint8_t* membase_ = nullptr;
  uint8_t* physbase_ = nullptr;
  uint64_t frame_ = 0;

  // Caches.
  std::unordered_map<uint64_t, std::unique_ptr<ShaderEntry>> shaders_;
  std::unordered_map<uint64_t, std::unique_ptr<RenderTarget>> render_targets_;
  std::unordered_map<uint64_t, std::unique_ptr<HostTexture>> textures_;
  std::unordered_map<uint32_t, std::unique_ptr<ResolvedTexture>> resolved_;
  std::unordered_map<uint64_t, std::unique_ptr<VertexBufferEntry>> vertex_buffers_;
  std::vector<std::unique_ptr<VertexBufferEntry>> transient_vbs_;
  size_t transient_next_ = 0;
  std::unordered_map<uint64_t, Com<ID3D11SamplerState>> samplers_;
  std::unordered_map<uint64_t, Com<ID3D11BlendState>> blend_states_;
  std::unordered_map<uint64_t, Com<ID3D11DepthStencilState>> depth_states_;
  std::unordered_map<uint64_t, Com<ID3D11RasterizerState>> raster_states_;

  // Per-draw resources.
  Com<ID3D11Buffer> cb_vs_, cb_ps_, cb_sys_, cb_blit_;
  Com<ID3D11Buffer> index_buffer_;
  uint32_t index_capacity_ = 0;
  Com<ID3D11GeometryShader> rect_gs_;
  Com<ID3D11VertexShader> blit_vs_;
  Com<ID3D11PixelShader> blit_ps_, blit_depth_ps_;
  Com<ID3D11SamplerState> linear_clamp_, point_clamp_;
  Com<ID3D11RasterizerState> blit_raster_;
  Com<ID3D11DepthStencilState> no_depth_;
  RenderTarget* bound_color_[4] = {};
  RenderTarget* bound_depth_ = nullptr;
  std::vector<uint32_t> index_scratch_;
  std::vector<uint8_t> upload_scratch_;
  std::mutex mutex_;
};

// --------------------------------------------------------------------------- device
bool RendererImpl::EnsureDevice() {
  if (device_) return EnsureSwapChain();
  if (device_failed_) return false;
  auto* runtime = rex::Runtime::instance();
  if (!runtime) return false;
  membase_ = runtime->memory()->virtual_membase();
  physbase_ = runtime->memory()->physical_membase();
  if (auto* window = runtime->display_window())
    hwnd_ = static_cast<HWND>(window->GetNativeWindowHandle());
  if (!hwnd_) return false;

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  if (std::getenv("STREET4_D3D_DEBUG")) flags |= D3D11_CREATE_DEVICE_DEBUG;
  D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
                                 D3D11_SDK_VERSION, device_.put(), nullptr, context_.put());
  if (FAILED(hr)) {
    REXLOG_ERROR("street4 renderer: D3D11CreateDevice failed ({:08X})", uint32_t(hr));
    device_failed_ = true;
    return false;
  }
  context_->QueryInterface(__uuidof(ID3D11DeviceContext1),
                           reinterpret_cast<void**>(context1_.put()));
  HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
  if (compiler) compile_ = reinterpret_cast<D3DCompileFn>(GetProcAddress(compiler, "D3DCompile"));
  if (!compile_) {
    REXLOG_ERROR("street4 renderer: d3dcompiler_47.dll not available");
    device_failed_ = true;
    return false;
  }
  cache_dir_ = runtime->cache_root().empty() ? std::filesystem::path("cache")
                                             : runtime->cache_root();
  cache_dir_ /= "street4_shaders";
  std::error_code ec;
  std::filesystem::create_directories(cache_dir_, ec);

  auto make_cb = [&](Com<ID3D11Buffer>& b, UINT size) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = size;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device_->CreateBuffer(&d, nullptr, b.put());
  };
  make_cb(cb_vs_, 4096);
  make_cb(cb_ps_, 4096);
  make_cb(cb_sys_, (sizeof(SystemConstants) + 15) & ~15u);
  make_cb(cb_blit_, 32);

  std::vector<uint8_t> code;
  if (Compile(kRectListGs, "main", "gs_5_0", code, "rect_list_gs"))
    device_->CreateGeometryShader(code.data(), code.size(), nullptr, rect_gs_.put());
  if (Compile(kBlitHlsl, "vs_main", "vs_5_0", code, "blit_vs"))
    device_->CreateVertexShader(code.data(), code.size(), nullptr, blit_vs_.put());
  if (Compile(kBlitHlsl, "ps_main", "ps_5_0", code, "blit_ps"))
    device_->CreatePixelShader(code.data(), code.size(), nullptr, blit_ps_.put());
  if (Compile(kBlitHlsl, "ps_depth", "ps_5_0", code, "blit_depth_ps"))
    device_->CreatePixelShader(code.data(), code.size(), nullptr, blit_depth_ps_.put());

  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  device_->CreateSamplerState(&sd, linear_clamp_.put());
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  device_->CreateSamplerState(&sd, point_clamp_.put());
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  device_->CreateRasterizerState(&rd, blit_raster_.put());
  D3D11_DEPTH_STENCIL_DESC dd{};
  dd.DepthEnable = FALSE;
  device_->CreateDepthStencilState(&dd, no_depth_.put());
  REXLOG_INFO("street4 renderer: Direct3D 11 device ready (native rendering, no GPU emulation)");
  return EnsureSwapChain();
}

bool RendererImpl::EnsureSwapChain() {
  RECT rc;
  if (!GetClientRect(hwnd_, &rc)) return false;
  uint32_t w = std::max<LONG>(rc.right - rc.left, 1), h = std::max<LONG>(rc.bottom - rc.top, 1);
  if (swap_chain_ && w == bb_width_ && h == bb_height_) return true;
  if (!swap_chain_) {
    Com<IDXGIDevice> dxgi_device;
    device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgi_device.put()));
    Com<IDXGIAdapter> adapter;
    dxgi_device->GetAdapter(adapter.put());
    Com<IDXGIFactory2> factory;
    adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory.put()));
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = w;
    d.Height = h;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(factory->CreateSwapChainForHwnd(device_.get(), hwnd_, &d, nullptr, nullptr,
                                               swap_chain_.put()))) {
      REXLOG_ERROR("street4 renderer: swap chain creation failed");
      return false;
    }
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
  } else {
    backbuffer_rtv_.reset();
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    swap_chain_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
  }
  Com<ID3D11Texture2D> bb;
  swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(bb.put()));
  device_->CreateRenderTargetView(bb.get(), nullptr, backbuffer_rtv_.put());
  bb_width_ = w;
  bb_height_ = h;
  return true;
}

bool RendererImpl::Compile(const std::string& source, const char* entry, const char* target,
                           std::vector<uint8_t>& out, const std::string& cache_name) {
  auto path = cache_dir_ / (cache_name + ".dxbc");
  if (!cache_name.empty()) {
    std::ifstream f(path, std::ios::binary);
    if (f) {
      out.assign(std::istreambuf_iterator<char>(f), {});
      if (!out.empty()) return true;
    }
  }
  Com<ID3DBlob> code, errors;
  HRESULT hr = compile_(source.data(), source.size(), cache_name.c_str(), nullptr, nullptr, entry,
                        target, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, code.put(), errors.put());
  if (FAILED(hr)) {
    REXLOG_ERROR("street4 renderer: shader {} failed to compile: {}", cache_name,
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
    std::ofstream(cache_dir_ / (cache_name + ".failed.hlsl")) << source;
    return false;
  }
  out.assign(static_cast<uint8_t*>(code->GetBufferPointer()),
             static_cast<uint8_t*>(code->GetBufferPointer()) + code->GetBufferSize());
  if (!cache_name.empty()) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
  }
  return true;
}

ShaderEntry* RendererImpl::GetShader(ShaderStage stage, const ShaderMicrocode& code,
                                     const ShaderKey& key) {
  if (!code.address || !code.dword_count || code.dword_count > 0x10000) return nullptr;
  const uint8_t* src = Virtual(code.address);
  uint64_t hash = Hash64(src, code.dword_count * 4, uint64_t(stage) + 1) ^ (key.Pack() * 0x9E37ull);
  auto it = shaders_.find(hash);
  if (it != shaders_.end()) return it->second->failed ? nullptr : it->second.get();
  auto entry = std::make_unique<ShaderEntry>();
  std::vector<uint32_t> dwords(code.dword_count);
  for (uint32_t i = 0; i < code.dword_count; ++i) dwords[i] = LoadBE32(src + i * 4);
  entry->meta = TranslateShader(stage, dwords.data(), code.dword_count, key);
  char name[64];
  snprintf(name, sizeof(name), "%s_%016llx", stage == ShaderStage::kVertex ? "vs" : "ps",
           static_cast<unsigned long long>(hash));
  if (entry->meta.ok && std::getenv("STREET4_DUMP_HLSL")) {
    std::ofstream(cache_dir_ / (std::string(name) + ".hlsl")) << entry->meta.hlsl;
    std::ofstream bin(cache_dir_ / (std::string(name) + ".ucode"), std::ios::binary);
    bin.write(reinterpret_cast<const char*>(src), std::streamsize(code.dword_count * 4));
  }
  std::vector<uint8_t> bytecode;
  if (!entry->meta.ok ||
      !Compile(entry->meta.hlsl, "main", stage == ShaderStage::kVertex ? "vs_5_0" : "ps_5_0",
               bytecode, name)) {
    if (!entry->meta.ok)
      REXLOG_ERROR("street4 renderer: shader {} translation failed: {}", name, entry->meta.error);
    entry->failed = true;
  } else if (stage == ShaderStage::kVertex) {
    device_->CreateVertexShader(bytecode.data(), bytecode.size(), nullptr, entry->vs.put());
  } else {
    device_->CreatePixelShader(bytecode.data(), bytecode.size(), nullptr, entry->ps.put());
  }
  auto* raw = entry.get();
  shaders_[hash] = std::move(entry);
  return raw->failed ? nullptr : raw;
}

// --------------------------------------------------------------------------- targets
RenderTarget* RendererImpl::GetRenderTarget(bool depth, uint32_t base, uint32_t format,
                                            uint32_t pitch, uint32_t height) {
  pitch = std::clamp<uint32_t>(pitch, 1, 8192);
  height = std::clamp<uint32_t>(height, 1, 8192);
  uint64_t key = uint64_t(base) | (uint64_t(format) << 12) | (uint64_t(pitch) << 20) |
                 (uint64_t(depth) << 40);
  auto& slot = render_targets_[key];
  if (slot && slot->height >= height) {
    slot->last_used = frame_;
    return slot.get();
  }
  auto rt = std::make_unique<RenderTarget>();
  rt->edram_base = base;
  rt->format = format;
  rt->pitch = pitch;
  rt->depth = depth;
  rt->width = pitch;
  rt->height = std::max(height, slot ? slot->height : 0u);
  D3D11_TEXTURE2D_DESC td{};
  td.Width = rt->width;
  td.Height = rt->height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  if (depth) {
    bool fdepth = format == 1;
    td.Format = fdepth ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    device_->CreateTexture2D(&td, nullptr, rt->texture.put());
    D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
    dv.Format = fdepth ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT;
    dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(rt->texture.get(), &dv, rt->dsv.put());
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = fdepth ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(rt->texture.get(), &sv, rt->srv.put());
    context_->ClearDepthStencilView(rt->dsv.get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
  } else {
    td.Format = ColorRtFormat(format);
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    device_->CreateTexture2D(&td, nullptr, rt->texture.put());
    device_->CreateRenderTargetView(rt->texture.get(), nullptr, rt->rtv.put());
    device_->CreateShaderResourceView(rt->texture.get(), nullptr, rt->srv.put());
    float zero[4] = {0, 0, 0, 0};
    context_->ClearRenderTargetView(rt->rtv.get(), zero);
  }
  if (!rt->texture) return nullptr;
  if (slot && slot->texture) {
    // Keep the old contents when growing.
    D3D11_BOX box{0, 0, 0, slot->width, slot->height, 1};
    context_->CopySubresourceRegion(rt->texture.get(), 0, 0, 0, 0, slot->texture.get(), 0, &box);
  }
  rt->last_used = frame_;
  slot = std::move(rt);
  return slot.get();
}

// --------------------------------------------------------------------------- textures
ResolvedTexture* RendererImpl::GetResolved(uint32_t address, uint32_t width, uint32_t height,
                                           uint32_t format, bool create) {
  auto it = resolved_.find(address);
  if (it != resolved_.end()) {
    auto* r = it->second.get();
    if (!create || (r->width >= width && r->height >= height &&
                    ResolveDestFormat(r->format) == ResolveDestFormat(format)))
      return r;
  }
  if (!create) return nullptr;
  auto r = std::make_unique<ResolvedTexture>();
  r->address = address;
  r->width = std::clamp<uint32_t>(width, 1, 8192);
  r->height = std::clamp<uint32_t>(height, 1, 8192);
  r->format = format;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = r->width;
  td.Height = r->height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.SampleDesc.Count = 1;
  td.Format = ResolveDestFormat(format);
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  device_->CreateTexture2D(&td, nullptr, r->texture.put());
  if (!r->texture) return nullptr;
  device_->CreateRenderTargetView(r->texture.get(), nullptr, r->rtv.put());
  device_->CreateShaderResourceView(r->texture.get(), nullptr, r->srv.put());
  auto* raw = r.get();
  resolved_[address] = std::move(r);
  return raw;
}

ID3D11ShaderResourceView* RendererImpl::GetTexture(const uint32_t fetch[6], TextureDimension dim,
                                                   float size_out[4]) {
  if ((fetch[0] & 3) != 2) return nullptr;  // not a valid texture fetch constant
  TextureDesc desc = TextureDescFromFetch(fetch);
  size_out[0] = float(desc.width);
  size_out[1] = float(desc.height);
  size_out[2] = float(desc.depth);
  size_out[3] = 0.0f;
  // Render-to-texture results first.
  if (desc.base_address) {
    if (auto* r = GetResolved(desc.base_address, 0, 0, 0, false)) {
      // Valid while the CPU has not rewritten the guest memory there.
      uint32_t sample = std::min<uint32_t>(4096, desc.width * desc.height);
      uint64_t h = Hash64(Physical(desc.base_address), sample);
      if (h == r->guest_hash && (dim == TextureDimension::k2D || dim == TextureDimension::k1D))
        return r->srv.get();
      if (h != r->guest_hash) resolved_.erase(desc.base_address);
    }
  }
  uint64_t key = Hash64(&desc, sizeof(desc));
  auto& slot = textures_[key];
  auto sample_hash = [&]() {
    uint32_t extent = GuestBaseExtentBytes(desc);
    uint32_t addr = desc.base_address ? desc.base_address : desc.mip_address;
    if (!extent || (addr & 0x1FFFFFFF) + extent > 0x20000000u) return uint64_t(0);
    // Full hash for small textures, strided samples for large ones.
    if (extent <= 256 * 1024) return Hash64(Physical(addr), extent);
    uint64_t h = 0;
    for (uint32_t i = 0; i < 64; ++i) {
      uint32_t off = uint32_t(uint64_t(extent - 512) * i / 63);
      h = Hash64(Physical(addr + off), 512, h);
    }
    return h;
  };
  if (slot && slot->validated_frame != frame_) {
    uint64_t h = sample_hash();
    if (h != slot->sample_hash) slot.reset();
    else slot->validated_frame = frame_;
  }
  if (!slot) {
    LoadedTexture loaded;
    if (!LoadTexture(desc, [this](uint32_t a) -> const uint8_t* { return Physical(a); }, loaded))
      return nullptr;
    auto t = std::make_unique<HostTexture>();
    t->desc = desc;
    t->cube = desc.dimension == 3;
    t->array_size = loaded.array_size;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = loaded.width;
    td.Height = loaded.height;
    td.MipLevels = loaded.mip_levels;
    td.ArraySize = loaded.array_size;
    td.SampleDesc.Count = 1;
    td.Format = loaded.format.dxgi;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = t->cube && loaded.array_size == 6 ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    bool block = loaded.format.block_width > 1;
    if (block) {
      // Block-compressed resources need dimensions that are multiples of 4 at level 0.
      td.Width = std::max<UINT>(td.Width, 1);
      td.Height = std::max<UINT>(td.Height, 1);
    }
    std::vector<D3D11_SUBRESOURCE_DATA> init(loaded.subresources.size());
    for (size_t i = 0; i < init.size(); ++i) {
      init[i].pSysMem = loaded.subresources[i].data.data();
      init[i].SysMemPitch = loaded.subresources[i].row_pitch;
      init[i].SysMemSlicePitch = loaded.subresources[i].slice_pitch;
    }
    static int texture_logs = 0;
    if (texture_logs++ < 60)
      REXLOG_INFO("street4 texture: addr={:08X} mip={:08X} fmt={} {}x{}x{} dim={} tiled={} pitch={} "
                  "endian={} levels={} packed={} dxgi={}",
                  desc.base_address, desc.mip_address, desc.format, desc.width, desc.height,
                  desc.depth, desc.dimension, desc.tiled, desc.pitch_div32 * 32, desc.endian,
                  loaded.mip_levels, desc.packed_mips, int(loaded.format.dxgi));
    if (FAILED(device_->CreateTexture2D(&td, init.data(), t->texture.put()))) {
      REXLOG_ERROR("street4 texture: CreateTexture2D failed for fmt {} {}x{}", desc.format,
                   td.Width, td.Height);
      return nullptr;
    }
    t->sample_hash = sample_hash();
    t->validated_frame = frame_;
    slot = std::move(t);
  }
  slot->last_used = frame_;
  auto* t = slot.get();
  D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
  D3D11_TEXTURE2D_DESC td;
  t->texture->GetDesc(&td);
  sv.Format = td.Format;
  switch (dim) {
    case TextureDimension::kCube:
      if (!t->cube) return nullptr;
      if (!t->srv_cube) {
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = td.MipLevels;
        device_->CreateShaderResourceView(t->texture.get(), &sv, t->srv_cube.put());
      }
      return t->srv_cube.get();
    case TextureDimension::k3D:
    case TextureDimension::kStacked:
      if (!t->srv_array) {
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sv.Texture2DArray.MipLevels = td.MipLevels;
        sv.Texture2DArray.ArraySize = td.ArraySize;
        device_->CreateShaderResourceView(t->texture.get(), &sv, t->srv_array.put());
      }
      return t->srv_array.get();
    default:
      if (td.ArraySize != 1) return nullptr;
      if (!t->srv2d) {
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = td.MipLevels;
        device_->CreateShaderResourceView(t->texture.get(), &sv, t->srv2d.put());
      }
      return t->srv2d.get();
  }
}

ID3D11SamplerState* RendererImpl::GetSampler(const uint32_t f[6]) {
  uint32_t clamp_x = (f[0] >> 10) & 7, clamp_y = (f[0] >> 13) & 7, clamp_z = (f[0] >> 16) & 7;
  uint32_t mag = (f[3] >> 19) & 3, min = (f[3] >> 21) & 3, mip = (f[3] >> 23) & 3;
  uint32_t aniso = (f[3] >> 25) & 7;
  uint32_t border = f[5] & 3;
  int32_t lod_bias = int32_t(f[4] << 10) >> 22;  // bits 12-21
  uint32_t mip_min = (f[4] >> 2) & 15, mip_max = (f[4] >> 6) & 15;
  uint64_t key = uint64_t(clamp_x) | (uint64_t(clamp_y) << 3) | (uint64_t(clamp_z) << 6) |
                 (uint64_t(mag) << 9) | (uint64_t(min) << 11) | (uint64_t(mip) << 13) |
                 (uint64_t(aniso) << 15) | (uint64_t(border) << 18) |
                 (uint64_t(uint32_t(lod_bias) & 0x3FF) << 20) | (uint64_t(mip_min) << 30) |
                 (uint64_t(mip_max) << 34);
  auto& s = samplers_[key];
  if (!s) {
    D3D11_SAMPLER_DESC d{};
    bool lmag = mag == 1, lmin = min == 1, lmip = mip == 1;
    if (aniso > 1 && aniso != 7) {
      d.Filter = D3D11_FILTER_ANISOTROPIC;
      d.MaxAnisotropy = 1u << (aniso - 1);
    } else {
      d.Filter = D3D11_ENCODE_BASIC_FILTER(lmin ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                           lmag ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                           lmip ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                           D3D11_FILTER_REDUCTION_TYPE_STANDARD);
    }
    d.AddressU = AddressMode(clamp_x);
    d.AddressV = AddressMode(clamp_y);
    d.AddressW = AddressMode(clamp_z);
    d.MipLODBias = float(lod_bias) / 32.0f;
    d.MinLOD = 0.0f;  // the host texture starts at the fetch constant's first stored level
    d.MaxLOD = mip == 2 ? 0.0f : D3D11_FLOAT32_MAX;
    float b = border == 1 ? 1.0f : 0.0f;
    d.BorderColor[0] = d.BorderColor[1] = d.BorderColor[2] = b;
    d.BorderColor[3] = border == 1 ? 1.0f : 0.0f;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    device_->CreateSamplerState(&d, s.put());
  }
  return s.get();
}

// --------------------------------------------------------------------------- buffers
void RendererImpl::UpdateDynamicBuffer(Com<ID3D11Buffer>& buffer, uint32_t& capacity, UINT bind,
                                       const void* data, uint32_t bytes) {
  if (!buffer || capacity < bytes) {
    capacity = std::max<uint32_t>(bytes, 64 * 1024);
    capacity = (capacity + 0xFFFF) & ~0xFFFFu;
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = capacity;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = bind;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (bind == D3D11_BIND_SHADER_RESOURCE) d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    device_->CreateBuffer(&d, nullptr, buffer.put());
  }
  D3D11_MAPPED_SUBRESOURCE m;
  if (SUCCEEDED(context_->Map(buffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
    std::memcpy(m.pData, data, bytes);
    context_->Unmap(buffer.get(), 0);
  }
}

ID3D11ShaderResourceView* RendererImpl::GetVertexBuffer(uint32_t physical, uint32_t bytes,
                                                        bool cacheable, const uint8_t* host) {
  bytes = std::min<uint32_t>((bytes + 15) & ~15u, 64u << 20);
  if (!bytes) bytes = 16;
  auto fill = [&](VertexBufferEntry& e) {
    UINT old_capacity = e.capacity;
    UpdateDynamicBuffer(e.buffer, e.capacity, D3D11_BIND_SHADER_RESOURCE, host, bytes);
    if (!e.srv || old_capacity != e.capacity) {
      D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
      sv.Format = DXGI_FORMAT_R32_TYPELESS;
      sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
      sv.BufferEx.NumElements = e.capacity / 4;
      sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
      device_->CreateShaderResourceView(e.buffer.get(), &sv, e.srv.put());
    }
  };
  if (!cacheable) {
    if (transient_next_ >= transient_vbs_.size())
      transient_vbs_.push_back(std::make_unique<VertexBufferEntry>());
    auto& e = *transient_vbs_[transient_next_++];
    fill(e);
    return e.srv.get();
  }
  uint64_t key = uint64_t(physical) | (uint64_t(bytes) << 32);
  auto& slot = vertex_buffers_[key];
  if (!slot) slot = std::make_unique<VertexBufferEntry>();
  auto& e = *slot;
  e.last_used = frame_;
  if (e.validated_frame == frame_ && e.srv) return e.srv.get();
  uint64_t h = Hash64(host, bytes);
  if (!e.srv || h != e.hash) {
    fill(e);
    e.hash = h;
  }
  e.validated_frame = frame_;
  return e.srv.get();
}

// --------------------------------------------------------------------------- primitives
bool RendererImpl::BuildIndices(const DrawRequest& draw, D3D11_PRIMITIVE_TOPOLOGY& topology,
                                std::vector<uint32_t>& out, bool& use_indices, bool& rect_list,
                                const GuestDevice& dev) {
  out.clear();
  rect_list = false;
  uint32_t n = draw.count;
  if (!n) return false;
  std::vector<uint32_t>& src = index_scratch_;
  src.resize(n);
  bool reset_enabled = (dev.reg(0x2205) >> 21) & 1;  // PA_SU_SC_MODE_CNTL.multi_prim_ib_ena
  uint32_t reset_index = dev.reg(0x2103) & 0xFFFFFF;
  if (draw.indexed) {
    if (!ValidGuest(draw.index_address)) return false;
    const uint8_t* p = Virtual(draw.index_address);
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t v = draw.index32 ? LoadBE32(p + i * 4) : LoadBE16(p + i * 2);
      bool is_reset = reset_enabled && (draw.index32 ? (v & 0xFFFFFF) == reset_index
                                                     : v == (reset_index & 0xFFFF));
      src[i] = is_reset ? 0xFFFFFFFFu : uint32_t(int32_t(v) + draw.base_vertex);
    }
  } else {
    for (uint32_t i = 0; i < n; ++i) src[i] = draw.first_vertex + i;
  }
  use_indices = true;
  switch (draw.primitive) {
    case 1: topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; out = src; break;
    case 2: topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; out = src; break;
    case 3: topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; out = src; break;
    case 4: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; out = src; break;
    case 6: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; out = src; break;
    case 5: case 15: {  // fan / polygon
      topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      uint32_t first = 0;
      for (uint32_t i = 0; i < n; ++i) {
        if (src[i] == 0xFFFFFFFFu) { first = i + 1; continue; }
        if (i >= first + 2) {
          out.push_back(src[first]);
          out.push_back(src[i - 1]);
          out.push_back(src[i]);
        }
      }
      break;
    }
    case 8:  // rectangle list: 3 vertices each, expanded by the geometry shader
      topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      out = src;
      out.resize(n / 3 * 3);
      rect_list = true;
      break;
    case 12:  // line loop
      topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
      out = src;
      out.push_back(src[0]);
      break;
    case 13:  // quad list
      topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      for (uint32_t i = 0; i + 3 < n; i += 4) {
        uint32_t q[6] = {src[i], src[i + 1], src[i + 2], src[i], src[i + 2], src[i + 3]};
        out.insert(out.end(), q, q + 6);
      }
      break;
    case 14:  // quad strip
      topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      for (uint32_t i = 0; i + 3 < n; i += 2) {
        uint32_t q[6] = {src[i], src[i + 1], src[i + 3], src[i], src[i + 3], src[i + 2]};
        out.insert(out.end(), q, q + 6);
      }
      break;
    default:
      return false;
  }
  return !out.empty();
}

// --------------------------------------------------------------------------- states
void RendererImpl::SetupStates(const GuestDevice& dev, uint32_t color_mask_active,
                               bool depth_active) {
  // Blend.
  uint32_t color_mask = dev.reg(0x2104);
  uint32_t blend[4] = {dev.reg(0x2201), dev.reg(0x2209), dev.reg(0x220A), dev.reg(0x220B)};
  uint32_t colorcontrol = dev.reg(0x2202);
  bool a2c = (colorcontrol >> 4) & 1;
  uint64_t bkey = Hash64(blend, sizeof(blend)) ^ (uint64_t(color_mask & color_mask_active) << 1) ^
                  uint64_t(a2c);
  auto& bs = blend_states_[bkey];
  if (!bs) {
    D3D11_BLEND_DESC d{};
    d.AlphaToCoverageEnable = a2c;
    d.IndependentBlendEnable = TRUE;
    for (int i = 0; i < 4; ++i) {
      auto& rt = d.RenderTarget[i];
      uint32_t b = blend[i];
      uint32_t cs = b & 31, cop = (b >> 5) & 7, cd = (b >> 8) & 31;
      uint32_t as = (b >> 16) & 31, aop = (b >> 21) & 7, ad = (b >> 24) & 31;
      rt.BlendEnable = !(cs == 1 && cd == 0 && cop == 0 && as == 1 && ad == 0 && aop == 0);
      rt.SrcBlend = BlendFactor(cs, false);
      rt.DestBlend = BlendFactor(cd, false);
      rt.BlendOp = BlendOp(cop);
      rt.SrcBlendAlpha = BlendFactor(as, true);
      rt.DestBlendAlpha = BlendFactor(ad, true);
      rt.BlendOpAlpha = BlendOp(aop);
      rt.RenderTargetWriteMask = UINT8((color_mask & color_mask_active) >> (4 * i)) & 0xF;
    }
    device_->CreateBlendState(&d, bs.put());
  }
  float blend_factor[4] = {dev.reg_float(0x2105), dev.reg_float(0x2106), dev.reg_float(0x2107),
                           dev.reg_float(0x2108)};
  context_->OMSetBlendState(bs.get(), blend_factor, 0xFFFFFFFF);

  // Depth / stencil.
  uint32_t dc = dev.reg(0x2200);
  uint32_t ref_front = dev.reg(0x210D), ref_back = dev.reg(0x210C);
  uint64_t dkey = uint64_t(dc) | (uint64_t(ref_front & 0xFFFF00) << 32) |
                  (uint64_t(depth_active) << 63);
  dkey ^= uint64_t(ref_back & 0xFFFF00) << 8;
  auto& ds = depth_states_[dkey];
  if (!ds) {
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = depth_active && ((dc >> 1) & 1);
    d.DepthWriteMask = ((dc >> 2) & 1) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = CompareFunc(dc >> 4);
    d.StencilEnable = depth_active && (dc & 1);
    d.StencilReadMask = UINT8(ref_front >> 8);
    d.StencilWriteMask = UINT8(ref_front >> 16);
    d.FrontFace.StencilFunc = CompareFunc(dc >> 8);
    d.FrontFace.StencilFailOp = StencilOp(dc >> 11);
    d.FrontFace.StencilPassOp = StencilOp(dc >> 14);
    d.FrontFace.StencilDepthFailOp = StencilOp(dc >> 17);
    if ((dc >> 7) & 1) {
      d.BackFace.StencilFunc = CompareFunc(dc >> 20);
      d.BackFace.StencilFailOp = StencilOp(dc >> 23);
      d.BackFace.StencilPassOp = StencilOp(dc >> 26);
      d.BackFace.StencilDepthFailOp = StencilOp(dc >> 29);
    } else {
      d.BackFace = d.FrontFace;
    }
    device_->CreateDepthStencilState(&d, ds.put());
  }
  context_->OMSetDepthStencilState(ds.get(), ref_front & 0xFF);

  // Rasterizer.
  uint32_t mode = dev.reg(0x2205);
  bool poly_offset = (mode >> 11) & 3;
  float offset_scale = poly_offset ? dev.reg_float(0x2380) : 0.0f;
  float offset = poly_offset ? dev.reg_float(0x2381) : 0.0f;
  uint32_t scale_bits, offset_bits;
  std::memcpy(&scale_bits, &offset_scale, 4);
  std::memcpy(&offset_bits, &offset, 4);
  uint64_t rkey = uint64_t(mode & 0x1F) | (uint64_t(scale_bits) << 5) ^ (uint64_t(offset_bits) << 31);
  auto& rs = raster_states_[rkey];
  if (!rs) {
    D3D11_RASTERIZER_DESC d{};
    d.FillMode = ((mode >> 3) & 3) && ((mode >> 5) & 7) == 1 ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    bool cull_front = mode & 1, cull_back = (mode >> 1) & 1;
    d.CullMode = cull_front && !cull_back   ? D3D11_CULL_FRONT
                 : cull_back && !cull_front ? D3D11_CULL_BACK
                                            : D3D11_CULL_NONE;
    d.FrontCounterClockwise = ((mode >> 2) & 1) == 0;
    d.DepthBias = INT(offset * 16777215.0f);
    d.SlopeScaledDepthBias = offset_scale / 16.0f;
    d.DepthClipEnable = TRUE;
    d.ScissorEnable = TRUE;
    device_->CreateRasterizerState(&d, rs.put());
  }
  context_->RSSetState(rs.get());
}

// --------------------------------------------------------------------------- draw
void RendererImpl::Draw(const GuestDevice& dev, const DrawRequest& draw) {
  if (!EnsureDevice()) return;
  if ((dev.reg(0x2208) & 7) != 4 && (dev.reg(0x2208) & 7) != 0) {
    // RB_MODECONTROL not in color+depth mode (copy/clear passes are handled by Resolve).
    return;
  }
  // Shaders.
  uint32_t program_cntl = dev.reg(0x2180);
  uint32_t context_misc = dev.reg(0x2181);
  ShaderKey vkey;
  vkey.rect_list = draw.primitive == 8;
  ShaderKey pkey;
  pkey.param_gen = (program_cntl >> 18) & 1;
  pkey.param_gen_register = uint8_t((context_misc >> 8) & 0xFF);
  ShaderEntry* vs = GetShader(ShaderStage::kVertex,
                              VertexShaderMicrocode(membase_, dev.u32(dev::kVertexShader)), vkey);
  ShaderEntry* ps = GetShader(ShaderStage::kPixel,
                              PixelShaderMicrocode(membase_, dev.u32(dev::kPixelShader)), pkey);
  static int draw_logs = 0;
  bool log_draw = draw_logs < 40 && frame_ < 3;
  if (log_draw) {
    ++draw_logs;
    REXLOG_INFO("street4 draw: prim={} count={} indexed={} vs={} ps={} surface={:08X} color0={:08X} "
                "depth={:08X} mask={:08X} modectl={:08X} vte={:08X}",
                draw.primitive, draw.count, draw.indexed, vs != nullptr, ps != nullptr,
                dev.reg(0x2000), dev.reg(0x2001), dev.reg(0x2002), dev.reg(0x2104), dev.reg(0x2208),
                dev.reg(0x2206));
  }
  if (!vs) return;

  // Render targets.
  uint32_t surface = dev.reg(0x2000);
  uint32_t pitch = surface & 0x3FFF;
  uint32_t window_br = dev.reg(0x2082);
  uint32_t height = std::max<uint32_t>((window_br >> 16) & 0x3FFF, 1);
  uint32_t width_scissor = window_br & 0x3FFF;
  if (!pitch) pitch = std::max<uint32_t>(width_scissor, 1);
  uint32_t color_mask = dev.reg(0x2104);
  uint32_t color_written = ps ? ps->meta.color_outputs : 0;
  uint32_t active_mask = 0;
  ID3D11RenderTargetView* rtvs[4] = {};
  const uint32_t color_info_regs[4] = {0x2001, 0x2003, 0x2004, 0x2005};
  float exp_scale[4] = {1, 1, 1, 1};
  for (int i = 0; i < 4; ++i) {
    bound_color_[i] = nullptr;
    if (!((color_mask >> (4 * i)) & 0xF) || !(color_written & (1u << i))) continue;
    uint32_t info = dev.reg(color_info_regs[i]);
    auto* rt = GetRenderTarget(false, info & 0xFFF, (info >> 16) & 0xF, pitch, height);
    if (!rt) continue;
    rtvs[i] = rt->rtv.get();
    bound_color_[i] = rt;
    active_mask |= 0xFu << (4 * i);
    int bias = int32_t(info << 6) >> 26;
    exp_scale[i] = std::ldexp(1.0f, bias);
  }
  uint32_t depthcontrol = dev.reg(0x2200);
  bool depth_active = (depthcontrol & 0x3) != 0 || ps == nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  bound_depth_ = nullptr;
  if ((depthcontrol & 0x3) != 0) {
    uint32_t info = dev.reg(0x2002);
    if (auto* rt = GetRenderTarget(true, info & 0xFFF, (info >> 16) & 1, pitch, height)) {
      dsv = rt->dsv.get();
      bound_depth_ = rt;
    }
  } else {
    depth_active = false;
  }
  if (!active_mask && !dsv) return;
  // Unbind any SRVs that alias the targets.
  ID3D11ShaderResourceView* null_srvs[32] = {};
  context_->PSSetShaderResources(0, 32, null_srvs);
  context_->OMSetRenderTargets(4, rtvs, dsv);

  // Geometry.
  D3D11_PRIMITIVE_TOPOLOGY topology;
  std::vector<uint32_t> indices;
  bool use_indices = false, rect_list = false;
  if (!BuildIndices(draw, topology, indices, use_indices, rect_list, dev)) return;
  UpdateDynamicBuffer(index_buffer_, index_capacity_, D3D11_BIND_INDEX_BUFFER, indices.data(),
                      uint32_t(indices.size() * 4));
  context_->IASetIndexBuffer(index_buffer_.get(), DXGI_FORMAT_R32_UINT, 0);
  context_->IASetPrimitiveTopology(topology);
  context_->IASetInputLayout(nullptr);

  // Constants.
  SystemConstants sys{};
  const uint8_t* bl = dev.bool_loop_constants();
  for (int i = 0; i < 8; ++i) sys.bools[i] = LoadBE32(bl + i * 4);
  for (int i = 0; i < 32; ++i) sys.loops[i] = LoadBE32(bl + 32 + i * 4);
  for (int i = 0; i < 96; ++i) sys.vfetch_endian[i] = LoadBE32(dev.vertex_fetch_constant(i) + 4) & 3;
  if (draw.vfetch_override) sys.vfetch_endian[draw.override_index] = draw.override_endian;
  sys.viewport[0] = dev.reg_float(0x210F);
  sys.viewport[1] = dev.reg_float(0x2110);
  sys.viewport[2] = dev.reg_float(0x2111);
  sys.viewport[3] = dev.reg_float(0x2112);
  sys.viewport_z[0] = dev.reg_float(0x2113);
  sys.viewport_z[1] = dev.reg_float(0x2114);
  sys.viewport_z[2] = float(pitch);
  sys.viewport_z[3] = float(height);
  sys.vte_flags = dev.reg(0x2206);
  sys.pixel_offset = (dev.reg(0x2302) & 1) ? 0.0f : 0.5f;
  uint32_t window_offset = dev.reg(0x2080);
  bool vtx_window_offset = (dev.reg(0x2205) >> 16) & 1;
  if (vtx_window_offset) {
    sys.window_offset_x = float(int32_t(window_offset << 17) >> 17);
    sys.window_offset_y = float(int32_t(window_offset << 1) >> 17);
  }
  sys.alpha_test = dev.reg(0x2202) & 0xF;
  sys.alpha_ref = dev.reg_float(0x210E);
  for (int i = 0; i < 4; ++i) sys.rt_exp_scale[i] = exp_scale[i];

  // Textures (both stages share slots 0-31).
  ID3D11ShaderResourceView* srvs[32] = {};
  ID3D11SamplerState* samps[32] = {};
  uint32_t tex_mask = vs->meta.tfetch_mask | (ps ? ps->meta.tfetch_mask : 0);
  for (uint32_t slot = 0; slot < 32; ++slot) {
    if (!(tex_mask & (1u << slot))) continue;
    uint32_t f[6];
    const uint8_t* fc = dev.fetch_constant(slot);
    for (int k = 0; k < 6; ++k) f[k] = LoadBE32(fc + k * 4);
    TextureDimension dim = (ps && (ps->meta.tfetch_mask & (1u << slot)))
                               ? ps->meta.tfetch_dimension[slot]
                               : vs->meta.tfetch_dimension[slot];
    srvs[slot] = GetTexture(f, dim, sys.tex_size[slot]);
    samps[slot] = GetSampler(f);
    sys.tex_swizzle[slot] = (f[3] >> 1) & 0xFFF;
    uint32_t sign_x = (f[0] >> 2) & 3;
    int32_t exp_adjust = int32_t(f[3] << 13) >> 26;
    sys.tex_flags[slot] = (sign_x == 2 ? 1u : 0u) | ((uint32_t(exp_adjust) & 0xFF) << 8);
  }

  // Vertex buffers.
  ID3D11ShaderResourceView* vbs[96] = {};
  for (size_t k = 0; k < vs->meta.vfetch_slots.size() && k < 96; ++k) {
    uint32_t index = vs->meta.vfetch_slots[k];
    if (draw.vfetch_override && index == draw.override_index) {
      if (!ValidGuest(draw.override_address)) return;
      vbs[k] = GetVertexBuffer(0, draw.override_bytes, false, Virtual(draw.override_address));
      continue;
    }
    const uint8_t* vf = dev.vertex_fetch_constant(index);
    uint32_t w0 = LoadBE32(vf), w1 = LoadBE32(vf + 4);
    uint32_t address = (w0 & ~3u);  // dwords << 2 == bytes (type in low bits)
    uint32_t bytes = ((w1 >> 2) & 0xFFFFFF) * 4;
    if ((address & 0x1FFFFFFF) + bytes > 0x20000000u) bytes = 0x20000000u - (address & 0x1FFFFFFF);
    vbs[k] = GetVertexBuffer(address, bytes, true, Physical(address));
  }

  auto upload_consts = [&](ID3D11Buffer* cb, const uint8_t* src) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(context_->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
      auto* d = static_cast<uint32_t*>(m.pData);
      for (int i = 0; i < 1024; ++i) d[i] = LoadBE32(src + i * 4);
      context_->Unmap(cb, 0);
    }
  };
  upload_consts(cb_vs_.get(), dev.vs_constants());
  upload_consts(cb_ps_.get(), dev.ps_constants());
  {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(context_->Map(cb_sys_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
      std::memcpy(m.pData, &sys, sizeof(sys));
      context_->Unmap(cb_sys_.get(), 0);
    }
  }
  ID3D11Buffer* cbs[3] = {cb_vs_.get(), cb_ps_.get(), cb_sys_.get()};
  context_->VSSetConstantBuffers(0, 3, cbs);
  context_->PSSetConstantBuffers(0, 3, cbs);
  context_->GSSetConstantBuffers(0, 3, cbs);
  context_->VSSetShaderResources(0, 32, srvs);
  context_->VSSetSamplers(0, 16, samps);
  if (!vs->meta.vfetch_slots.empty())
    context_->VSSetShaderResources(32, UINT(std::min<size_t>(vs->meta.vfetch_slots.size(), 96)), vbs);
  context_->PSSetShaderResources(0, 32, srvs);
  context_->PSSetSamplers(0, 16, samps);
  context_->VSSetShader(vs->vs.get(), nullptr, 0);
  context_->GSSetShader(rect_list ? rect_gs_.get() : nullptr, nullptr, 0);
  context_->PSSetShader(ps ? ps->ps.get() : nullptr, nullptr, 0);

  SetupStates(dev, active_mask, depth_active);

  // Viewport covers the whole surface; the guest viewport is applied in the shader.
  D3D11_VIEWPORT vp{0, 0, float(pitch), float(height), 0.0f, 1.0f};
  context_->RSSetViewports(1, &vp);
  uint32_t tl = dev.reg(0x2081), br = dev.reg(0x2082);
  LONG sx0 = LONG(tl & 0x3FFF), sy0 = LONG((tl >> 16) & 0x3FFF);
  LONG sx1 = LONG(br & 0x3FFF), sy1 = LONG((br >> 16) & 0x3FFF);
  if (!(tl >> 31)) {
    LONG ox = int32_t(window_offset << 17) >> 17, oy = int32_t(window_offset << 1) >> 17;
    sx0 += ox; sx1 += ox; sy0 += oy; sy1 += oy;
  }
  uint32_t stl = dev.reg(0x200E), sbr = dev.reg(0x200F);
  sx0 = std::max<LONG>(sx0, int32_t(stl << 17) >> 17);
  sy0 = std::max<LONG>(sy0, int32_t(stl << 1) >> 17);
  if (sbr & 0x7FFF) sx1 = std::min<LONG>(sx1, int32_t(sbr << 17) >> 17);
  if ((sbr >> 16) & 0x7FFF) sy1 = std::min<LONG>(sy1, int32_t(sbr << 1) >> 17);
  D3D11_RECT scissor{std::max<LONG>(sx0, 0), std::max<LONG>(sy0, 0),
                     std::min<LONG>(sx1, LONG(pitch)), std::min<LONG>(sy1, LONG(height))};
  if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) return;
  context_->RSSetScissorRects(1, &scissor);

  if (std::getenv("STREET4_DUMP_DRAW") && ps) {
    static std::string dumped;
    char ps_name[32];
    snprintf(ps_name, sizeof(ps_name), "%p", static_cast<void*>(ps));
    if (dumped.find(ps_name) == std::string::npos && dumped.size() < 4000) {
      dumped += ps_name;
      std::string c;
      for (int i = 0; i < 12; ++i) {
        const uint8_t* v = dev.vs_constants() + i * 16;
        c += fmt::format(" c{}=({:g},{:g},{:g},{:g})", i, AsFloat(LoadBE32(v)), AsFloat(LoadBE32(v + 4)),
                         AsFloat(LoadBE32(v + 8)), AsFloat(LoadBE32(v + 12)));
      }
      std::string verts;
      if (draw.vfetch_override) {
        const uint8_t* d = Virtual(draw.override_address);
        for (uint32_t i = 0; i < std::min<uint32_t>(draw.override_bytes / 4, 24); ++i)
          verts += fmt::format(" {:g}", AsFloat(LoadBE32(d + i * 4)));
      }
      REXLOG_INFO("street4 drawdump ps={} prim={} count={} vp=({:g},{:g},{:g},{:g},{:g},{:g}) vte={:08X} "
                  "rt={}x{} tex={:08X}{} verts:{}",
                  ps_name, draw.primitive, draw.count, sys.viewport[0], sys.viewport[1],
                  sys.viewport[2], sys.viewport[3], sys.viewport_z[0], sys.viewport_z[1],
                  sys.vte_flags, pitch, height, tex_mask, c, verts);
    }
  }
  context_->DrawIndexed(UINT(indices.size()), 0, 0);
}

// --------------------------------------------------------------------------- blit
void RendererImpl::BlitTo(ID3D11RenderTargetView* rtv, uint32_t, uint32_t, D3D11_VIEWPORT vp,
                          ID3D11ShaderResourceView* src, float, float, float u0, float v0,
                          float u1, float v1, bool depth) {
  float cb[8] = {u0, v0, u1 - u0, v1 - v0, 1.0f, 0, 0, 0};
  D3D11_MAPPED_SUBRESOURCE m;
  if (SUCCEEDED(context_->Map(cb_blit_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
    std::memcpy(m.pData, cb, sizeof(cb));
    context_->Unmap(cb_blit_.get(), 0);
  }
  ID3D11ShaderResourceView* null_srvs[32] = {};
  context_->PSSetShaderResources(0, 32, null_srvs);
  context_->OMSetRenderTargets(1, &rtv, nullptr);
  context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  context_->OMSetDepthStencilState(no_depth_.get(), 0);
  context_->RSSetState(blit_raster_.get());
  context_->RSSetViewports(1, &vp);
  D3D11_RECT r{LONG(vp.TopLeftX), LONG(vp.TopLeftY), LONG(vp.TopLeftX + vp.Width),
               LONG(vp.TopLeftY + vp.Height)};
  context_->RSSetScissorRects(1, &r);
  context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context_->IASetInputLayout(nullptr);
  context_->VSSetShader(blit_vs_.get(), nullptr, 0);
  context_->GSSetShader(nullptr, nullptr, 0);
  context_->PSSetShader(depth ? blit_depth_ps_.get() : blit_ps_.get(), nullptr, 0);
  ID3D11Buffer* b = cb_blit_.get();
  context_->VSSetConstantBuffers(0, 1, &b);
  context_->PSSetConstantBuffers(0, 1, &b);
  context_->PSSetShaderResources(0, 1, &src);
  ID3D11SamplerState* s = linear_clamp_.get();
  context_->PSSetSamplers(0, 1, &s);
  context_->Draw(3, 0);
  context_->PSSetShaderResources(0, 32, null_srvs);
}

// --------------------------------------------------------------------------- resolve
void RendererImpl::Resolve(const GuestDevice& dev, const ResolveRequest& req) {
  if (!EnsureDevice()) return;
  uint32_t copy_control = dev.reg(0x2318);
  uint32_t src_select = copy_control & 7;
  bool clear_color = (copy_control >> 8) & 1 || (req.flags & 0x100);
  bool clear_depth = (copy_control >> 9) & 1 || (req.flags & 0x200);
  uint32_t copy_command = (copy_control >> 20) & 3;
  uint32_t surface = dev.reg(0x2000);
  uint32_t pitch = std::max<uint32_t>(surface & 0x3FFF, 1);
  uint32_t window_br = dev.reg(0x2082);
  uint32_t height = std::max<uint32_t>((window_br >> 16) & 0x3FFF, 1);

  RenderTarget* src = nullptr;
  if (src_select == 4) {
    uint32_t info = dev.reg(0x2002);
    src = GetRenderTarget(true, info & 0xFFF, (info >> 16) & 1, pitch, height);
  } else {
    const uint32_t regs[4] = {0x2001, 0x2003, 0x2004, 0x2005};
    uint32_t info = dev.reg(regs[src_select & 3]);
    src = GetRenderTarget(false, info & 0xFFF, (info >> 16) & 0xF, pitch, height);
  }
  if (!src) return;
  int32_t x0 = req.src_x0, y0 = req.src_y0, x1 = req.src_x1, y1 = req.src_y1;
  if (x1 < 0) { x0 = 0; y0 = 0; x1 = int32_t(src->width); y1 = int32_t(height); }
  x1 = std::min<int32_t>(x1, int32_t(src->width));
  y1 = std::min<int32_t>(y1, int32_t(src->height));

  if (copy_command != 3 && req.has_dest && x1 > x0 && y1 > y0) {
    uint32_t dest_base = dev.reg(0x2319);
    uint32_t dest_pitch_reg = dev.reg(0x231A);
    uint32_t dest_info = dev.reg(0x231B);
    uint32_t dest_pitch = std::max<uint32_t>(dest_pitch_reg & 0x3FFF, 1);
    uint32_t dest_height = std::max<uint32_t>((dest_pitch_reg >> 16) & 0x3FFF, 1);
    uint32_t dest_format = (dest_info >> 7) & 0x3F;
    if (src->depth) dest_format = 36;  // depth -> 32-bit float texture
    if (frame_ < 5)
      REXLOG_INFO("street4 resolve: src rt base={} fmt={} depth={} rect=({},{})-({},{}) -> dest "
                  "{:08X} pitch={} h={} fmt={} at ({},{})",
                  src->edram_base, src->format, src->depth, x0, y0, x1, y1, dest_base, dest_pitch,
                  dest_height, dest_format, req.dst_x, req.dst_y);
    if (auto* dst = GetResolved(dest_base & 0x1FFFFFFF, dest_pitch, dest_height, dest_format, true)) {
      D3D11_VIEWPORT vp{float(req.dst_x), float(req.dst_y), float(x1 - x0), float(y1 - y0), 0, 1};
      float sw = float(src->width), sh = float(src->height);
      BlitTo(dst->rtv.get(), dst->width, dst->height, vp, src->srv.get(), sw, sh, x0 / sw, y0 / sh,
             x1 / sw, y1 / sh, src->depth);
      // Remember what the CPU-visible memory looked like, to detect CPU overwrites.
      uint32_t sample = std::min<uint32_t>(4096, dest_pitch * dest_height);
      dst->guest_hash = Hash64(Physical(dest_base), sample);
      dst->frame = frame_;
    }
  }
  if (clear_color && !src->depth && context1_) {
    uint32_t c = dev.reg(0x231E), lo = dev.reg(0x231F);
    float rgba[4];
    switch (src->format) {
      case 0: case 1:
        for (int i = 0; i < 4; ++i) rgba[i] = float((c >> (8 * i)) & 0xFF) / 255.0f;
        break;
      case 2: case 10:
        rgba[0] = float(c & 0x3FF) / 1023.0f;
        rgba[1] = float((c >> 10) & 0x3FF) / 1023.0f;
        rgba[2] = float((c >> 20) & 0x3FF) / 1023.0f;
        rgba[3] = float(c >> 30) / 3.0f;
        break;
      case 14:
        rgba[0] = AsFloat(c); rgba[1] = rgba[2] = rgba[3] = 0;
        break;
      default: {
        // 64-bit formats: high dword holds the first two halves.
        auto h2f = [](uint32_t h) {
          uint32_t s = (h & 0x8000) << 16, e = (h >> 10) & 31, m = h & 0x3FF;
          uint32_t f = e == 0 ? s : e == 31 ? (s | 0x7F800000 | (m << 13))
                                            : (s | ((e + 112) << 23) | (m << 13));
          return AsFloat(f);
        };
        rgba[0] = h2f(c >> 16); rgba[1] = h2f(c & 0xFFFF);
        rgba[2] = h2f(lo >> 16); rgba[3] = h2f(lo & 0xFFFF);
      }
    }
    D3D11_RECT r{x0, y0, x1, y1};
    context1_->ClearView(src->rtv.get(), rgba, &r, 1);
  }
  if (clear_depth) {
    uint32_t info = dev.reg(0x2002);
    if (auto* d = GetRenderTarget(true, info & 0xFFF, (info >> 16) & 1, pitch, height)) {
      uint32_t dv = dev.reg(0x231D);
      float z = d->format == 1 ? 0.0f : float(dv >> 8) / 16777215.0f;
      if (d->format == 1) {
        uint32_t f24 = dv >> 8, m = f24 & 0xFFFFF, e = f24 >> 20;
        z = e ? (1.0f + m / 1048576.0f) * std::ldexp(1.0f, int(e) - 15) : m / 1048576.0f * std::ldexp(1.0f, -14);
      }
      context_->ClearDepthStencilView(d->dsv.get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                      std::clamp(z, 0.0f, 1.0f), UINT8(dv & 0xFF));
    }
  }
}

void RendererImpl::ClearBound(const GuestDevice& dev, uint32_t color_targets,
                              const float rgba[4], bool depth, float z, bool stencil, uint8_t s,
                              const int32_t* rects, uint32_t rect_count) {
  if (!EnsureDevice()) return;
  uint32_t surface = dev.reg(0x2000);
  uint32_t pitch = std::max<uint32_t>(surface & 0x3FFF, 1);
  uint32_t height = std::max<uint32_t>((dev.reg(0x2082) >> 16) & 0x3FFF, 1);
  std::vector<D3D11_RECT> r;
  for (uint32_t i = 0; i < rect_count; ++i)
    r.push_back({rects[i * 4], rects[i * 4 + 1], rects[i * 4 + 2], rects[i * 4 + 3]});
  const uint32_t regs[4] = {0x2001, 0x2003, 0x2004, 0x2005};
  for (int i = 0; i < 4; ++i) {
    if (!(color_targets & (1u << i))) continue;
    uint32_t info = dev.reg(regs[i]);
    auto* rt = GetRenderTarget(false, info & 0xFFF, (info >> 16) & 0xF, pitch, height);
    if (!rt) continue;
    if (!r.empty() && context1_)
      context1_->ClearView(rt->rtv.get(), rgba, r.data(), UINT(r.size()));
    else
      context_->ClearRenderTargetView(rt->rtv.get(), rgba);
  }
  if (depth || stencil) {
    uint32_t info = dev.reg(0x2002);
    if (auto* d = GetRenderTarget(true, info & 0xFFF, (info >> 16) & 1, pitch, height)) {
      // Depth-stencil views cannot be cleared per rectangle in D3D11; rectangles that
      // cover less than the surface are rare for depth and cleared fully.
      context_->ClearDepthStencilView(
          d->dsv.get(), (depth ? D3D11_CLEAR_DEPTH : 0) | (stencil ? D3D11_CLEAR_STENCIL : 0),
          std::clamp(z, 0.0f, 1.0f), s);
    }
  }
}

// --------------------------------------------------------------------------- present
void RendererImpl::Present(const uint8_t* fetch) {
  if (!EnsureDevice()) return;
  uint32_t f[6];
  for (int k = 0; k < 6; ++k) f[k] = LoadBE32(fetch + k * 4);
  TextureDesc desc = TextureDescFromFetch(f);
  float clear[4] = {0, 0, 0, 1};
  context_->ClearRenderTargetView(backbuffer_rtv_.get(), clear);
  ID3D11ShaderResourceView* src = nullptr;
  float sw = float(desc.width), sh = float(desc.height);
  // The front buffer header holds a CPU virtual address (full 20-bit page field); the
  // resolve wrote to its GPU physical address.
  uint32_t front_virtual = (f[1] >> 12) << 12;
  if (auto* r = GetResolved(VirtualToPhysical(front_virtual) & 0x1FFFFFFF, 0, 0, 0, false)) {
    src = r->srv.get();
    sw = float(r->width);
    sh = float(r->height);
  } else if (auto* r2 = GetResolved(desc.base_address & 0x1FFFFFFF, 0, 0, 0, false)) {
    src = r2->srv.get();
    sw = float(r2->width);
    sh = float(r2->height);
  }
  if (frame_ < 5 || (frame_ % 600) == 0)
    REXLOG_INFO("street4 present #{}: front base={:08X} {}x{} fmt={} found={}", frame_,
                desc.base_address, desc.width, desc.height, desc.format, src != nullptr);
  static const char* shot_dir = std::getenv("STREET4_SHOT_DIR");
  static const int shot_every = std::getenv("STREET4_SHOT_EVERY") ? std::atoi(std::getenv("STREET4_SHOT_EVERY")) : 0;
  bool take_shot = src && shot_dir && shot_every > 0 && frame_ > 0 && (frame_ % shot_every) == 0;
  if (src && ((frame_ % 120) == 0 || take_shot)) {
    // Diagnostics: average color of the guest front buffer.
    Com<ID3D11Resource> res;
    src->GetResource(res.put());
    D3D11_TEXTURE2D_DESC td;
    static_cast<ID3D11Texture2D*>(res.get())->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    Com<ID3D11Texture2D> staging;
    if (td.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
        SUCCEEDED(device_->CreateTexture2D(&td, nullptr, staging.put()))) {
      context_->CopyResource(staging.get(), res.get());
      D3D11_MAPPED_SUBRESOURCE m;
      if (SUCCEEDED(context_->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) {
        uint64_t sum[4] = {};
        uint32_t n = 0;
        for (uint32_t y = 0; y < td.Height; y += 16)
          for (uint32_t x = 0; x < td.Width; x += 16, ++n) {
            const uint8_t* p = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch + x * 4;
            for (int c = 0; c < 4; ++c) sum[c] += p[c];
          }
        if (take_shot) {
          // Diagnostics: write the guest front buffer as a 24-bit BMP.
          char name[512];
          snprintf(name, sizeof(name), "%s/frame-%06llu.bmp", shot_dir,
                   static_cast<unsigned long long>(frame_));
          std::ofstream f(name, std::ios::binary);
          uint32_t w = td.Width, h = td.Height, row = (w * 3 + 3) & ~3u;
          uint32_t size = 54 + row * h;
          uint8_t hdr[54] = {'B', 'M'};
          std::memcpy(hdr + 2, &size, 4);
          uint32_t off = 54, dib = 40, planes_bpp = 1 | (24 << 16);
          std::memcpy(hdr + 10, &off, 4);
          std::memcpy(hdr + 14, &dib, 4);
          std::memcpy(hdr + 18, &w, 4);
          std::memcpy(hdr + 22, &h, 4);
          std::memcpy(hdr + 26, &planes_bpp, 4);
          f.write(reinterpret_cast<char*>(hdr), 54);
          std::vector<uint8_t> line(row);
          for (uint32_t y = 0; y < h; ++y) {
            const uint8_t* p = static_cast<const uint8_t*>(m.pData) + (h - 1 - y) * m.RowPitch;
            for (uint32_t x = 0; x < w; ++x) {
              line[x * 3] = p[x * 4 + 2];
              line[x * 3 + 1] = p[x * 4 + 1];
              line[x * 3 + 2] = p[x * 4];
            }
            f.write(reinterpret_cast<char*>(line.data()), row);
          }
        }
        context_->Unmap(staging.get(), 0);
        REXLOG_INFO("street4 frame {}: front average rgba=({}, {}, {}, {})", frame_, sum[0] / n,
                    sum[1] / n, sum[2] / n, sum[3] / n);
      }
    }
  }
  if (src) {
    // Letterbox to the guest aspect ratio.
    float gw = float(desc.width), gh = float(desc.height);
    float scale = std::min(float(bb_width_) / gw, float(bb_height_) / gh);
    float w = gw * scale, h = gh * scale;
    D3D11_VIEWPORT vp{(float(bb_width_) - w) * 0.5f, (float(bb_height_) - h) * 0.5f, w, h, 0, 1};
    BlitTo(backbuffer_rtv_.get(), bb_width_, bb_height_, vp, src, sw, sh, 0, 0, gw / sw, gh / sh,
           false);
  }
  static const bool vsync = [] {
    // PC options written by fifastreet4_options.exe (Vsync=0/1), next to the executable.
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring ini(path, n);
    ini = ini.substr(0, ini.find_last_of(std::wstring{wchar_t(92), L'/'}) + 1) + L"fifastreet4-pc.ini";
    return GetPrivateProfileIntW(L"PC", L"Vsync", 1, ini.c_str()) != 0;
  }();
  swap_chain_->Present(vsync ? 1 : 0, 0);
  ++frame_;
  transient_next_ = 0;
  // Drop resources unused for a while.
  if ((frame_ & 255) == 0) {
    for (auto it = textures_.begin(); it != textures_.end();)
      it = (frame_ - it->second->last_used > 600) ? textures_.erase(it) : std::next(it);
    for (auto it = vertex_buffers_.begin(); it != vertex_buffers_.end();)
      it = (frame_ - it->second->last_used > 600) ? vertex_buffers_.erase(it) : std::next(it);
  }
}

}  // namespace

// --------------------------------------------------------------------------- facade
static RendererImpl& Impl() {
  static RendererImpl impl;
  return impl;
}

Renderer& Renderer::Get() {
  static Renderer r;
  return r;
}
void Renderer::Draw(const GuestDevice& d, const DrawRequest& r) { Impl().Draw(d, r); }
void Renderer::Resolve(const GuestDevice& d, const ResolveRequest& r) { Impl().Resolve(d, r); }
void Renderer::ClearBound(const GuestDevice& d, uint32_t c, const float rgba[4], bool z_, float z,
                          bool s_, uint8_t s, const int32_t* rects, uint32_t rect_count) {
  Impl().ClearBound(d, c, rgba, z_, z, s_, s, rects, rect_count);
}
void Renderer::Present(const uint8_t* fetch) { Impl().Present(fetch); }
uint64_t Renderer::frame() const { return Impl().frame(); }

}  // namespace street4
