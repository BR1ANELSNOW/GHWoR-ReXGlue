#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#define REXLOG_ERROR(...) ((void)0)
#define REXLOG_INFO(...) ((void)0)

namespace rex::graphics {
enum class NativeGuestOutputBackend : uint32_t { kUnknown = 0, kD3D12, kVulkan };
struct NativeGuestOutputRenderContext {
  NativeGuestOutputBackend backend;
  uint32_t guest_output_width;
  uint32_t guest_output_height;
  uint32_t display_width;
  uint32_t display_height;
  struct D3D12Context {
    void* command_processor;
    void* command_processor_user_data;
    ID3D12Device* device;
    ID3D12Resource* guest_output_resource;
    DXGI_FORMAT guest_output_format;
    D3D12_RESOURCE_STATES guest_output_initial_state;
    bool (*request_one_use_view_descriptor)(void*, D3D12_CPU_DESCRIPTOR_HANDLE*, D3D12_GPU_DESCRIPTOR_HANDLE*);
    bool (*create_root_signature)(void*, const D3D12_ROOT_SIGNATURE_DESC*, ID3D12RootSignature**);
    bool (*push_transition_barrier)(void*, ID3D12Resource*, D3D12_RESOURCE_STATES, D3D12_RESOURCE_STATES);
    void (*submit_barriers)(void*);
    void (*copy_texture_region)(void*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
                                const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
    void (*clear_render_target_view)(void*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT[4]);
    void (*clear_unordered_access_view_float)(void*, D3D12_GPU_DESCRIPTOR_HANDLE,
                                              D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*,
                                              const FLOAT[4]);
    void (*set_graphics_root_signature)(void*, ID3D12RootSignature*);
    void (*set_graphics_root_32bit_constants)(void*, UINT, UINT, const void*, UINT);
    void (*set_graphics_root_descriptor_table)(void*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
    void (*set_pipeline_state)(void*, ID3D12PipelineState*);
    void (*ia_set_primitive_topology)(void*, D3D12_PRIMITIVE_TOPOLOGY);
    void (*ia_set_vertex_buffers)(void*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
    void (*om_set_render_targets)(void*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                  const D3D12_CPU_DESCRIPTOR_HANDLE*);
    void (*rs_set_viewport)(void*, const D3D12_VIEWPORT*);
    void (*rs_set_scissor_rect)(void*, const D3D12_RECT*);
    void (*draw_instanced)(void*, UINT, UINT, UINT, UINT);
  } d3d12;
};
namespace {
struct HighwayVertex {
  float x;
  float y;
  uint8_t color[4];
  float u;
  float v;
};
static_assert(sizeof(HighwayVertex) == 20);

struct HighwayReplayResources {
  ID3D12Device* device;
  DXGI_FORMAT format;
  ID3D12RootSignature* root_signature;
  ID3D12PipelineState* pipeline_state;
  ID3D12DescriptorHeap* rtv_heap;
  ID3D12Resource* vertex_buffer;
  ID3D12Resource* texture;
  ID3D12Resource* texture_upload;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv;
  D3D12_VERTEX_BUFFER_VIEW vertex_view;
  bool texture_uploaded;
};

HighwayReplayResources g_highway{};

void ResetHighwayResources() {
  if (g_highway.texture_upload != nullptr) g_highway.texture_upload->Release();
  if (g_highway.texture != nullptr) g_highway.texture->Release();
  if (g_highway.vertex_buffer != nullptr) g_highway.vertex_buffer->Release();
  if (g_highway.pipeline_state != nullptr) g_highway.pipeline_state->Release();
  if (g_highway.root_signature != nullptr) g_highway.root_signature->Release();
  if (g_highway.rtv_heap != nullptr) g_highway.rtv_heap->Release();
  g_highway = {};
}

constexpr char kHighwayShader[] = R"(
cbuffer OutputSize : register(b0) {
  float2 output_size;
};
Texture2D highway_texture : register(t0);
SamplerState highway_sampler : register(s0);

struct VSInput {
  float2 position : POSITION;
  float4 color : COLOR0;
  float2 uv : TEXCOORD0;
};
struct VSOutput {
  float4 position : SV_Position;
  float4 color : COLOR0;
  float2 uv : TEXCOORD0;
};

VSOutput VSMain(VSInput input) {
  VSOutput output;
  output.position = float4(input.position.x * (2.0 / output_size.x) - 1.0,
                           1.0 - input.position.y * (2.0 / output_size.y), 0.0, 1.0);
  output.color = input.color;
  output.uv = input.uv;
  return output;
}

float4 PSMain(VSOutput input) : SV_Target0 {
  float4 texel = highway_texture.Sample(highway_sampler, input.uv);
  return float4(texel.rgb * input.color.rgb, 0.0);
}
)";

bool ReadExactFile(const char* path, void* output, size_t expected_size) {
  std::FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    REXLOG_ERROR("GHWOR NATIVE HIGHWAY REPLAY: missing asset {}", path);
    return false;
  }
  const size_t read_size = std::fread(output, 1, expected_size, file);
  const int trailing_byte = std::fgetc(file);
  std::fclose(file);
  if (read_size != expected_size || trailing_byte != EOF) {
    REXLOG_ERROR("GHWOR NATIVE HIGHWAY REPLAY: invalid asset size {} expected={}", path,
                 expected_size);
    return false;
  }
  return true;
}

bool CompileHighwayShader(const char* entry_point, const char* target, ID3DBlob** shader_out) {
  ID3DBlob* errors = nullptr;
  const HRESULT result = D3DCompile(kHighwayShader, std::strlen(kHighwayShader),
                                    "GHWoRNativeHighway", nullptr, nullptr, entry_point, target,
                                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, shader_out, &errors);
  if (FAILED(result)) {
    REXLOG_ERROR("GHWOR NATIVE HIGHWAY REPLAY: shader {} failed{}", entry_point,
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
    if (errors != nullptr) errors->Release();
    return false;
  }
  if (errors != nullptr) errors->Release();
  return true;
}

bool EnsureHighwayResources(const NativeGuestOutputRenderContext::D3D12Context& d3d12) {
  if (g_highway.device == d3d12.device && g_highway.format == d3d12.guest_output_format &&
      g_highway.root_signature != nullptr && g_highway.pipeline_state != nullptr &&
      g_highway.vertex_buffer != nullptr && g_highway.texture != nullptr) {
    return true;
  }
  ResetHighwayResources();

  D3D12_DESCRIPTOR_RANGE srv_range{};
  srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  srv_range.NumDescriptors = 1;
  srv_range.BaseShaderRegister = 0;
  srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  D3D12_ROOT_PARAMETER root_parameters[2]{};
  root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  root_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
  root_parameters[0].DescriptorTable.pDescriptorRanges = &srv_range;
  root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  root_parameters[1].Constants.ShaderRegister = 0;
  root_parameters[1].Constants.Num32BitValues = 2;
  root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  D3D12_STATIC_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC root_desc{};
  root_desc.NumParameters = 2;
  root_desc.pParameters = root_parameters;
  root_desc.NumStaticSamplers = 1;
  root_desc.pStaticSamplers = &sampler;
  root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  if (!d3d12.create_root_signature(d3d12.command_processor_user_data, &root_desc,
                                   &g_highway.root_signature)) {
    ResetHighwayResources();
    return false;
  }

  ID3DBlob* vs = nullptr;
  ID3DBlob* ps = nullptr;
  if (!CompileHighwayShader("VSMain", "vs_5_0", &vs) ||
      !CompileHighwayShader("PSMain", "ps_5_0", &ps)) {
    if (vs != nullptr) vs->Release();
    if (ps != nullptr) ps->Release();
    ResetHighwayResources();
    return false;
  }
  const D3D12_INPUT_ELEMENT_DESC input_layout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
  pipeline.pRootSignature = g_highway.root_signature;
  pipeline.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  pipeline.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  pipeline.BlendState.RenderTarget[0].BlendEnable = TRUE;
  pipeline.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
  pipeline.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
  pipeline.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
  pipeline.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ZERO;
  pipeline.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
  pipeline.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
  pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pipeline.SampleMask = UINT_MAX;
  pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pipeline.RasterizerState.DepthClipEnable = TRUE;
  pipeline.DepthStencilState.DepthEnable = FALSE;
  pipeline.DepthStencilState.StencilEnable = FALSE;
  pipeline.InputLayout = {input_layout, 3};
  pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pipeline.NumRenderTargets = 1;
  pipeline.RTVFormats[0] = d3d12.guest_output_format;
  pipeline.SampleDesc.Count = 1;
  HRESULT hr = d3d12.device->CreateGraphicsPipelineState(&pipeline,
                                                         IID_PPV_ARGS(&g_highway.pipeline_state));
  vs->Release();
  ps->Release();
  if (FAILED(hr)) {
    REXLOG_ERROR("GHWOR NATIVE HIGHWAY REPLAY: pipeline creation failed 0x{:08X}", uint32_t(hr));
    ResetHighwayResources();
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
  rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_desc.NumDescriptors = 1;
  if (FAILED(d3d12.device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&g_highway.rtv_heap)))) {
    ResetHighwayResources();
    return false;
  }
  g_highway.rtv = g_highway.rtv_heap->GetCPUDescriptorHandleForHeapStart();

  D3D12_HEAP_PROPERTIES upload_heap{};
  upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC buffer_desc{};
  buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer_desc.Width = 48000;
  buffer_desc.Height = 1;
  buffer_desc.DepthOrArraySize = 1;
  buffer_desc.MipLevels = 1;
  buffer_desc.SampleDesc.Count = 1;
  buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(d3d12.device->CreateCommittedResource(
          &upload_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
          nullptr, IID_PPV_ARGS(&g_highway.vertex_buffer)))) {
    ResetHighwayResources();
    return false;
  }
  void* mapping = nullptr;
  if (FAILED(g_highway.vertex_buffer->Map(0, nullptr, &mapping))) {
    ResetHighwayResources();
    return false;
  }
  if (!ReadExactFile("GHWoR_Highway_VB_Native.bin", mapping, 48000)) {
    g_highway.vertex_buffer->Unmap(0, nullptr);
    ResetHighwayResources();
    return false;
  }
  g_highway.vertex_buffer->Unmap(0, nullptr);
  g_highway.vertex_view.BufferLocation = g_highway.vertex_buffer->GetGPUVirtualAddress();
  g_highway.vertex_view.SizeInBytes = 48000;
  g_highway.vertex_view.StrideInBytes = sizeof(HighwayVertex);

  D3D12_RESOURCE_DESC texture_desc{};
  texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texture_desc.Width = 512;
  texture_desc.Height = 1024;
  texture_desc.DepthOrArraySize = 1;
  texture_desc.MipLevels = 1;
  texture_desc.Format = DXGI_FORMAT_BC3_UNORM;
  texture_desc.SampleDesc.Count = 1;
  D3D12_HEAP_PROPERTIES default_heap{};
  default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  if (FAILED(d3d12.device->CreateCommittedResource(
          &default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST,
          nullptr, IID_PPV_ARGS(&g_highway.texture)))) {
    ResetHighwayResources();
    return false;
  }
  buffer_desc.Width = 524288;
  if (FAILED(d3d12.device->CreateCommittedResource(
          &upload_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
          nullptr, IID_PPV_ARGS(&g_highway.texture_upload)))) {
    ResetHighwayResources();
    return false;
  }
  mapping = nullptr;
  if (FAILED(g_highway.texture_upload->Map(0, nullptr, &mapping))) {
    ResetHighwayResources();
    return false;
  }
  if (!ReadExactFile("GHWoR_Highway_Texture_BC3_Linear.bin", mapping, 524288)) {
    g_highway.texture_upload->Unmap(0, nullptr);
    ResetHighwayResources();
    return false;
  }
  g_highway.texture_upload->Unmap(0, nullptr);
  g_highway.device = d3d12.device;
  g_highway.format = d3d12.guest_output_format;
  REXLOG_INFO("GHWOR NATIVE HIGHWAY REPLAY: resources loaded vertices=2400 texture=512x1024 BC3");
  return true;
}

bool RenderNativeHighwayReplay(const NativeGuestOutputRenderContext& context) {
  const auto& d3d12 = context.d3d12;
  if (!EnsureHighwayResources(d3d12)) return false;
  if (!g_highway.texture_uploaded) {
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = g_highway.texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = g_highway.texture_upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_BC3_UNORM;
    src.PlacedFootprint.Footprint.Width = 512;
    src.PlacedFootprint.Footprint.Height = 1024;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = 2048;
    d3d12.copy_texture_region(d3d12.command_processor_user_data, &dst, 0, 0, 0, &src, nullptr);
    d3d12.push_transition_barrier(d3d12.command_processor_user_data, g_highway.texture,
                                  D3D12_RESOURCE_STATE_COPY_DEST,
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    d3d12.submit_barriers(d3d12.command_processor_user_data);
    g_highway.texture_uploaded = true;
    REXLOG_INFO("GHWOR NATIVE HIGHWAY REPLAY: texture uploaded to D3D12");
  }

  D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu{};
  if (!d3d12.request_one_use_view_descriptor(d3d12.command_processor_user_data, &srv_cpu,
                                              &srv_gpu)) return false;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = DXGI_FORMAT_BC3_UNORM;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  d3d12.device->CreateShaderResourceView(g_highway.texture, &srv, srv_cpu);
  d3d12.device->CreateRenderTargetView(d3d12.guest_output_resource, nullptr, g_highway.rtv);
  d3d12.push_transition_barrier(d3d12.command_processor_user_data,
                                d3d12.guest_output_resource, d3d12.guest_output_initial_state,
                                D3D12_RESOURCE_STATE_RENDER_TARGET);
  d3d12.submit_barriers(d3d12.command_processor_user_data);

  const D3D12_VIEWPORT viewport = {0.0f, 0.0f, float(context.guest_output_width),
                                   float(context.guest_output_height), 0.0f, 1.0f};
  const D3D12_RECT scissor = {0, 0, LONG(context.guest_output_width),
                              LONG(context.guest_output_height)};
  const float output_size[2] = {float(context.guest_output_width),
                                float(context.guest_output_height)};
  d3d12.set_graphics_root_signature(d3d12.command_processor_user_data,
                                    g_highway.root_signature);
  d3d12.set_pipeline_state(d3d12.command_processor_user_data, g_highway.pipeline_state);
  d3d12.set_graphics_root_descriptor_table(d3d12.command_processor_user_data, 0, srv_gpu);
  d3d12.set_graphics_root_32bit_constants(d3d12.command_processor_user_data, 1, 2,
                                          output_size, 0);
  d3d12.ia_set_primitive_topology(d3d12.command_processor_user_data,
                                  D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  d3d12.ia_set_vertex_buffers(d3d12.command_processor_user_data, 0, 1,
                              &g_highway.vertex_view);
  d3d12.om_set_render_targets(d3d12.command_processor_user_data, 1, &g_highway.rtv, FALSE,
                              nullptr);
  d3d12.rs_set_viewport(d3d12.command_processor_user_data, &viewport);
  d3d12.rs_set_scissor_rect(d3d12.command_processor_user_data, &scissor);
  d3d12.draw_instanced(d3d12.command_processor_user_data, 2400, 1, 0, 0);
  d3d12.push_transition_barrier(d3d12.command_processor_user_data,
                                d3d12.guest_output_resource, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                d3d12.guest_output_initial_state);
  d3d12.submit_barriers(d3d12.command_processor_user_data);
  return true;
}

}  // namespace
}  // namespace rex::graphics

extern "C" __declspec(dllexport) bool GHWoRNativeHighwayRender(
    const rex::graphics::NativeGuestOutputRenderContext* context) {
  if (context == nullptr ||
      context->backend != rex::graphics::NativeGuestOutputBackend::kD3D12) {
    return false;
  }
  const auto& d3d12 = context->d3d12;
  if (d3d12.device == nullptr || d3d12.guest_output_resource == nullptr ||
      d3d12.request_one_use_view_descriptor == nullptr ||
      d3d12.create_root_signature == nullptr || d3d12.push_transition_barrier == nullptr ||
      d3d12.submit_barriers == nullptr || d3d12.copy_texture_region == nullptr ||
      d3d12.set_graphics_root_signature == nullptr ||
      d3d12.set_graphics_root_32bit_constants == nullptr ||
      d3d12.set_graphics_root_descriptor_table == nullptr ||
      d3d12.set_pipeline_state == nullptr || d3d12.ia_set_primitive_topology == nullptr ||
      d3d12.ia_set_vertex_buffers == nullptr || d3d12.om_set_render_targets == nullptr ||
      d3d12.rs_set_viewport == nullptr || d3d12.rs_set_scissor_rect == nullptr ||
      d3d12.draw_instanced == nullptr) {
    return false;
  }
  return rex::graphics::RenderNativeHighwayReplay(*context);
}
