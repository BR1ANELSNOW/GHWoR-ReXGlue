#pragma once

#include <cstdint>

#if REX_HAS_D3D12
#include <rex/ui/d3d12/d3d12_api.h>
#endif

namespace rex::graphics::d3d12 {
class D3D12CommandProcessor;
}

namespace rex::graphics {

enum class NativeGuestOutputBackend : uint32_t {
  kUnknown = 0,
  kD3D12,
  kVulkan,
};

// First-stage Native GPU bridge. It exposes the presenter-owned guest output
// and deferred D3D12 recording operations without replacing the emulated path
// unless a registered renderer explicitly returns true.
struct NativeGuestOutputRenderContext {
  NativeGuestOutputBackend backend = NativeGuestOutputBackend::kUnknown;
  uint32_t guest_output_width = 0;
  uint32_t guest_output_height = 0;
  uint32_t display_width = 0;
  uint32_t display_height = 0;

#if REX_HAS_D3D12
  struct D3D12Context {
    d3d12::D3D12CommandProcessor* command_processor = nullptr;
    void* command_processor_user_data = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12Resource* guest_output_resource = nullptr;
    DXGI_FORMAT guest_output_format = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES guest_output_initial_state = D3D12_RESOURCE_STATE_COMMON;
    bool (*request_one_use_view_descriptor)(void* user_data,
                                            D3D12_CPU_DESCRIPTOR_HANDLE* cpu_out,
                                            D3D12_GPU_DESCRIPTOR_HANDLE* gpu_out) = nullptr;
    bool (*create_root_signature)(void* user_data, const D3D12_ROOT_SIGNATURE_DESC* desc,
                                  ID3D12RootSignature** root_signature_out) = nullptr;
    bool (*push_transition_barrier)(void* user_data, ID3D12Resource* resource,
                                    D3D12_RESOURCE_STATES old_state,
                                    D3D12_RESOURCE_STATES new_state) = nullptr;
    void (*submit_barriers)(void* user_data) = nullptr;
    void (*copy_texture_region)(void* user_data, const D3D12_TEXTURE_COPY_LOCATION* dst,
                                UINT dst_x, UINT dst_y, UINT dst_z,
                                const D3D12_TEXTURE_COPY_LOCATION* src,
                                const D3D12_BOX* src_box) = nullptr;
    void (*clear_render_target_view)(void* user_data, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                                     const FLOAT color[4]) = nullptr;
    void (*clear_unordered_access_view_float)(void* user_data,
                                              D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
                                              D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
                                              ID3D12Resource* resource,
                                              const FLOAT values[4]) = nullptr;
    void (*set_graphics_root_signature)(void* user_data,
                                        ID3D12RootSignature* root_signature) = nullptr;
    void (*set_graphics_root_32bit_constants)(void* user_data, UINT root_parameter_index,
                                              UINT num_32bit_values_to_set,
                                              const void* src_data,
                                              UINT dest_offset_in_32bit_values) = nullptr;
    void (*set_graphics_root_descriptor_table)(void* user_data, UINT root_parameter_index,
                                               D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor) =
        nullptr;
    void (*set_pipeline_state)(void* user_data, ID3D12PipelineState* pipeline_state) = nullptr;
    void (*ia_set_primitive_topology)(void* user_data,
                                      D3D12_PRIMITIVE_TOPOLOGY primitive_topology) = nullptr;
    void (*ia_set_vertex_buffers)(void* user_data, UINT start_slot, UINT num_views,
                                  const D3D12_VERTEX_BUFFER_VIEW* views) = nullptr;
    void (*om_set_render_targets)(void* user_data, UINT num_render_target_descriptors,
                                  const D3D12_CPU_DESCRIPTOR_HANDLE* render_target_descriptors,
                                  BOOL rts_single_handle_to_descriptor_range,
                                  const D3D12_CPU_DESCRIPTOR_HANDLE* depth_stencil_descriptor) =
        nullptr;
    void (*rs_set_viewport)(void* user_data, const D3D12_VIEWPORT* viewport) = nullptr;
    void (*rs_set_scissor_rect)(void* user_data, const D3D12_RECT* rect) = nullptr;
    void (*draw_instanced)(void* user_data, UINT vertex_count_per_instance, UINT instance_count,
                           UINT start_vertex_location, UINT start_instance_location) = nullptr;
  } d3d12;
#endif
};

using NativeGuestOutputRenderer = bool (*)(const NativeGuestOutputRenderContext& context,
                                           void* user_data);

void SetNativeGuestOutputRenderer(NativeGuestOutputRenderer renderer, void* user_data);
bool TryRenderNativeGuestOutput(const NativeGuestOutputRenderContext& context);

}  // namespace rex::graphics
