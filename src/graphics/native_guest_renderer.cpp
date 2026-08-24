#include <rex/graphics/native_guest_renderer.h>

#include <atomic>
#include <climits>
#include <cstring>
#include <mutex>

#if REX_HAS_D3D12
#include <d3dcompiler.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>
REXCVAR_DEFINE_BOOL(native_guest_output_test_clear, false, "GPU/Native",
                    "Replace the presented guest frame with a solid native D3D12 test color")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(native_guest_output_test_triangle, false, "GPU/Native",
                    "Replace the presented guest frame with a native D3D12 RGB triangle")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
namespace rex::graphics {
namespace {

std::atomic<NativeGuestOutputRenderer> g_renderer{nullptr};
std::atomic<void*> g_renderer_user_data{nullptr};
std::atomic<bool> g_external_replay_frame{false};
#if REX_HAS_D3D12
using ExternalHighwayRender = bool (*)(const NativeGuestOutputRenderContext* context);
HMODULE g_external_highway_module = nullptr;
ExternalHighwayRender g_external_highway_render = nullptr;

bool RenderExternalHighway(const NativeGuestOutputRenderContext& context) {
  if (g_external_highway_render == nullptr) {
    g_external_highway_module = LoadLibraryW(L"ghwor-native-highway.dll");
    if (g_external_highway_module == nullptr) {
      REXLOG_ERROR("GHWOR NATIVE HIGHWAY EXTERNAL: LoadLibrary failed error={}",
                   uint32_t(GetLastError()));
      return false;
    }
    g_external_highway_render = reinterpret_cast<ExternalHighwayRender>(
        GetProcAddress(g_external_highway_module, "GHWoRNativeHighwayRender"));
    if (g_external_highway_render == nullptr) {
      REXLOG_ERROR("GHWOR NATIVE HIGHWAY EXTERNAL: export missing error={}",
                   uint32_t(GetLastError()));
      return false;
    }
    REXLOG_INFO("GHWOR NATIVE HIGHWAY EXTERNAL: module loaded after gameplay draw");
  }
  return g_external_highway_render(&context);
}
#endif

#if REX_HAS_D3D12

struct TriangleResources {
  ID3D12Device* device = nullptr;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  ID3D12RootSignature* root_signature = nullptr;
  ID3D12PipelineState* pipeline_state = nullptr;
  ID3D12DescriptorHeap* rtv_heap = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv{};

  void Reset() {
    if (pipeline_state != nullptr) {
      pipeline_state->Release();
      pipeline_state = nullptr;
    }
    if (root_signature != nullptr) {
      root_signature->Release();
      root_signature = nullptr;
    }
    if (rtv_heap != nullptr) {
      rtv_heap->Release();
      rtv_heap = nullptr;
    }
    device = nullptr;
    format = DXGI_FORMAT_UNKNOWN;
    rtv.ptr = 0;
  }

  ~TriangleResources() { Reset(); }
};

TriangleResources g_triangle;
std::mutex g_triangle_mutex;

constexpr char kTriangleShader[] = R"(
struct VSOutput {
  float4 position : SV_Position;
  float3 color : COLOR0;
};

VSOutput VSMain(uint vertex_id : SV_VertexID) {
  VSOutput output;
  if (vertex_id == 0) {
    output.position = float4(-0.68, -0.58, 0.0, 1.0);
    output.color = float3(1.0, 0.08, 0.05);
  } else if (vertex_id == 1) {
    output.position = float4(0.0, 0.68, 0.0, 1.0);
    output.color = float3(0.05, 1.0, 0.12);
  } else {
    output.position = float4(0.68, -0.58, 0.0, 1.0);
    output.color = float3(0.08, 0.22, 1.0);
  }
  return output;
}

float4 PSMain(VSOutput input) : SV_Target0 {
  return float4(input.color, 1.0);
}
)";

bool CompileTriangleShader(const char* entry_point, const char* target, ID3DBlob** shader_out) {
  ID3DBlob* errors = nullptr;
  const HRESULT result = D3DCompile(kTriangleShader, std::strlen(kTriangleShader),
                                    "GHWoRNativeTriangle", nullptr, nullptr, entry_point, target,
                                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, shader_out, &errors);
  if (FAILED(result)) {
    if (errors != nullptr) {
      REXLOG_ERROR("GHWOR NATIVE GPU TRIANGLE: shader {} failed: {}", entry_point,
                   static_cast<const char*>(errors->GetBufferPointer()));
      errors->Release();
    } else {
      REXLOG_ERROR("GHWOR NATIVE GPU TRIANGLE: shader {} failed (HRESULT 0x{:08X})",
                   entry_point, uint32_t(result));
    }
    return false;
  }
  if (errors != nullptr) {
    errors->Release();
  }
  return true;
}

bool EnsureTriangleResources(const NativeGuestOutputRenderContext::D3D12Context& d3d12) {
  std::lock_guard<std::mutex> lock(g_triangle_mutex);
  if (g_triangle.device == d3d12.device && g_triangle.format == d3d12.guest_output_format &&
      g_triangle.root_signature != nullptr && g_triangle.pipeline_state != nullptr &&
      g_triangle.rtv_heap != nullptr) {
    return true;
  }

  g_triangle.Reset();

  D3D12_ROOT_SIGNATURE_DESC root_desc{};
  root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  if (!d3d12.create_root_signature(d3d12.command_processor_user_data, &root_desc,
                                   &g_triangle.root_signature)) {
    REXLOG_ERROR("GHWOR NATIVE GPU TRIANGLE: root signature creation failed");
    g_triangle.Reset();
    return false;
  }

  ID3DBlob* vertex_shader = nullptr;
  ID3DBlob* pixel_shader = nullptr;
  if (!CompileTriangleShader("VSMain", "vs_5_0", &vertex_shader) ||
      !CompileTriangleShader("PSMain", "ps_5_0", &pixel_shader)) {
    if (vertex_shader != nullptr) vertex_shader->Release();
    if (pixel_shader != nullptr) pixel_shader->Release();
    g_triangle.Reset();
    return false;
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_desc{};
  pipeline_desc.pRootSignature = g_triangle.root_signature;
  pipeline_desc.VS = {vertex_shader->GetBufferPointer(), vertex_shader->GetBufferSize()};
  pipeline_desc.PS = {pixel_shader->GetBufferPointer(), pixel_shader->GetBufferSize()};
  pipeline_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
      D3D12_COLOR_WRITE_ENABLE_ALL;
  pipeline_desc.SampleMask = UINT_MAX;
  pipeline_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pipeline_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pipeline_desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
  pipeline_desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
  pipeline_desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
  pipeline_desc.RasterizerState.DepthClipEnable = TRUE;
  pipeline_desc.DepthStencilState.DepthEnable = FALSE;
  pipeline_desc.DepthStencilState.StencilEnable = FALSE;
  pipeline_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pipeline_desc.NumRenderTargets = 1;
  pipeline_desc.RTVFormats[0] = d3d12.guest_output_format;
  pipeline_desc.SampleDesc.Count = 1;

  const HRESULT pipeline_result = d3d12.device->CreateGraphicsPipelineState(
      &pipeline_desc, IID_PPV_ARGS(&g_triangle.pipeline_state));
  vertex_shader->Release();
  pixel_shader->Release();
  if (FAILED(pipeline_result)) {
    REXLOG_ERROR("GHWOR NATIVE GPU TRIANGLE: pipeline creation failed (HRESULT 0x{:08X})",
                 uint32_t(pipeline_result));
    g_triangle.Reset();
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
  rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_heap_desc.NumDescriptors = 1;
  if (FAILED(d3d12.device->CreateDescriptorHeap(&rtv_heap_desc,
                                                IID_PPV_ARGS(&g_triangle.rtv_heap)))) {
    REXLOG_ERROR("GHWOR NATIVE GPU TRIANGLE: RTV heap creation failed");
    g_triangle.Reset();
    return false;
  }

  g_triangle.rtv = g_triangle.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  g_triangle.device = d3d12.device;
  g_triangle.format = d3d12.guest_output_format;
  REXLOG_INFO("GHWOR NATIVE GPU TRIANGLE: D3D12 pipeline initialized format={}",
              uint32_t(g_triangle.format));
  return true;
}

bool RenderTestClear(const NativeGuestOutputRenderContext::D3D12Context& d3d12) {
  D3D12_CPU_DESCRIPTOR_HANDLE uav_cpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE uav_gpu{};
  if (!d3d12.request_one_use_view_descriptor(d3d12.command_processor_user_data, &uav_cpu,
                                              &uav_gpu)) {
    REXLOG_ERROR("GHWOR NATIVE GPU TEST CLEAR: failed to allocate a UAV descriptor");
    return false;
  }

  D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
  uav_desc.Format = d3d12.guest_output_format;
  uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  d3d12.device->CreateUnorderedAccessView(d3d12.guest_output_resource, nullptr, &uav_desc,
                                          uav_cpu);
  d3d12.push_transition_barrier(d3d12.command_processor_user_data,
                                d3d12.guest_output_resource,
                                d3d12.guest_output_initial_state,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  d3d12.submit_barriers(d3d12.command_processor_user_data);
  const FLOAT clear_color[4] = {0.02f, 0.08f, 0.80f, 1.0f};
  d3d12.clear_unordered_access_view_float(d3d12.command_processor_user_data, uav_gpu, uav_cpu,
                                          d3d12.guest_output_resource, clear_color);
  d3d12.push_transition_barrier(d3d12.command_processor_user_data,
                                d3d12.guest_output_resource,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                d3d12.guest_output_initial_state);
  d3d12.submit_barriers(d3d12.command_processor_user_data);
  return true;
}

bool RenderTestTriangle(const NativeGuestOutputRenderContext& context) {
  const auto& d3d12 = context.d3d12;
  if (!EnsureTriangleResources(d3d12)) {
    return false;
  }

  d3d12.device->CreateRenderTargetView(d3d12.guest_output_resource, nullptr, g_triangle.rtv);
  d3d12.push_transition_barrier(d3d12.command_processor_user_data,
                                d3d12.guest_output_resource,
                                d3d12.guest_output_initial_state,
                                D3D12_RESOURCE_STATE_RENDER_TARGET);
  d3d12.submit_barriers(d3d12.command_processor_user_data);

  const FLOAT background[4] = {0.008f, 0.012f, 0.035f, 1.0f};
  d3d12.clear_render_target_view(d3d12.command_processor_user_data, g_triangle.rtv, background);

  const D3D12_VIEWPORT viewport = {0.0f, 0.0f, float(context.guest_output_width),
                                   float(context.guest_output_height), 0.0f, 1.0f};
  const D3D12_RECT scissor = {0, 0, LONG(context.guest_output_width),
                              LONG(context.guest_output_height)};
  d3d12.set_graphics_root_signature(d3d12.command_processor_user_data,
                                    g_triangle.root_signature);
  d3d12.set_pipeline_state(d3d12.command_processor_user_data, g_triangle.pipeline_state);
  d3d12.ia_set_primitive_topology(d3d12.command_processor_user_data,
                                  D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  d3d12.om_set_render_targets(d3d12.command_processor_user_data, 1, &g_triangle.rtv, FALSE,
                              nullptr);
  d3d12.rs_set_viewport(d3d12.command_processor_user_data, &viewport);
  d3d12.rs_set_scissor_rect(d3d12.command_processor_user_data, &scissor);
  d3d12.draw_instanced(d3d12.command_processor_user_data, 3, 1, 0, 0);

  d3d12.push_transition_barrier(d3d12.command_processor_user_data,
                                d3d12.guest_output_resource,
                                D3D12_RESOURCE_STATE_RENDER_TARGET,
                                d3d12.guest_output_initial_state);
  d3d12.submit_barriers(d3d12.command_processor_user_data);
  return true;
}

#endif  // REX_HAS_D3D12

}  // namespace

void SetNativeGuestHighwayExternalReplayFrame(bool active) {
  g_external_replay_frame.store(active, std::memory_order_release);
}

void SetNativeGuestOutputRenderer(NativeGuestOutputRenderer renderer, void* user_data) {
  g_renderer_user_data.store(user_data, std::memory_order_release);
  g_renderer.store(renderer, std::memory_order_release);
}

bool TryRenderNativeGuestOutput(const NativeGuestOutputRenderContext& context) {
  NativeGuestOutputRenderer renderer = g_renderer.load(std::memory_order_acquire);
  const bool test_triangle = REXCVAR_GET(native_guest_output_test_triangle);
  const bool test_clear = REXCVAR_GET(native_guest_output_test_clear) && !test_triangle;
  const bool external_replay = false;
#if REX_HAS_D3D12
  if (external_replay) {
    static std::atomic<bool> external_success_logged{false};
    const bool rendered = RenderExternalHighway(context);
    if (rendered && !external_success_logged.exchange(true)) {
      REXLOG_INFO("GHWOR NATIVE HIGHWAY EXTERNAL: first native frame rendered");
    }
    return rendered;
  }
  if (test_triangle || test_clear) {
    const auto& d3d12 = context.d3d12;
    if (context.backend != NativeGuestOutputBackend::kD3D12 || d3d12.device == nullptr ||
        d3d12.guest_output_resource == nullptr ||
        d3d12.request_one_use_view_descriptor == nullptr ||
        d3d12.create_root_signature == nullptr || d3d12.push_transition_barrier == nullptr ||
        d3d12.submit_barriers == nullptr || d3d12.clear_render_target_view == nullptr ||
        d3d12.clear_unordered_access_view_float == nullptr ||
        d3d12.set_graphics_root_signature == nullptr || d3d12.set_pipeline_state == nullptr ||
        d3d12.ia_set_primitive_topology == nullptr || d3d12.om_set_render_targets == nullptr ||
        d3d12.rs_set_viewport == nullptr || d3d12.rs_set_scissor_rect == nullptr ||
        d3d12.draw_instanced == nullptr) {
      static std::atomic<bool> invalid_context_logged{false};
      if (!invalid_context_logged.exchange(true)) {
        REXLOG_ERROR("GHWOR NATIVE GPU: incomplete D3D12 bridge context");
      }
      return false;
    }
    return test_triangle ? RenderTestTriangle(context) : RenderTestClear(d3d12);
  }
#endif

  if (renderer == nullptr) {
    return false;
  }
  void* user_data = g_renderer_user_data.load(std::memory_order_acquire);
  return renderer(context, user_data);
}

}  // namespace rex::graphics
