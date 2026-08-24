#pragma once

#include <cstdint>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/ui/d3d12/d3d12_api.h>

namespace rex::ui::d3d12 { class D3D12Provider; }
namespace rex::graphics::d3d12 {

class DeferredCommandList;

class D3D12ZPDQueryPool {
 public:
  D3D12ZPDQueryPool() = default;
  D3D12ZPDQueryPool(const D3D12ZPDQueryPool&) = delete;
  D3D12ZPDQueryPool& operator=(const D3D12ZPDQueryPool&) = delete;
  ~D3D12ZPDQueryPool() { Shutdown(); }

  bool EnsureInitialized(const ui::d3d12::D3D12Provider& provider,
                         uint32_t requested_capacity, bool can_recreate,
                         bool initialize_rov_counter);
  void Shutdown();

  bool rtv_initialized() const {
    return query_heap_ && readback_buffer_ && readback_mapping_ != nullptr &&
           capacity_ != 0;
  }

  uint32_t capacity() const { return capacity_; }

  bool has_pending_resolve_batch() const {
    return !resolve_batch_indices_.empty() ||
           !rov_counter_resolve_batch_indices_.empty();
  }

  bool rov_initialized() const {
    return rov_counter_buffer_ && rov_counter_readback_buffer_ &&
           rov_counter_readback_mapping_ != nullptr && capacity_ != 0;
  }

  ID3D12Resource* rov_counter_buffer() const {
    return rov_counter_buffer_.Get();
  }

  bool has_free_indices() const { return !free_indices_.empty(); }

  bool AcquireQueryIndex(uint32_t& query_index, uint32_t& query_generation);
  void ReleaseQueryIndex(uint32_t query_index, uint32_t query_generation);
  bool GenerationMatches(uint32_t query_index, uint32_t query_generation) const;

  void BeginQuery(DeferredCommandList& deferred_command_list,
                  uint32_t query_index) const;
  void EndQuery(DeferredCommandList& deferred_command_list,
                uint32_t query_index) const;
  void QueueQueryResolve(uint32_t query_index, bool uses_rov_counter);
  void ClearROVCounter(DeferredCommandList& deferred_command_list,
                       uint32_t query_index) const;

  void FlushResolveBatch(DeferredCommandList& deferred_command_list,
                         bool submission_open);

  uint64_t GetQueryReadbackValue(uint32_t query_index,
                                 bool uses_rov_counter) const;

 private:
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> query_heap_;

  // Persistently mapped. Results readable once the fence signals.
  Microsoft::WRL::ComPtr<ID3D12Resource> readback_buffer_;
  uint64_t* readback_mapping_ = nullptr;

  Microsoft::WRL::ComPtr<ID3D12Resource> rov_counter_buffer_;
  Microsoft::WRL::ComPtr<ID3D12Resource> rov_counter_readback_buffer_;
  uint32_t* rov_counter_readback_mapping_ = nullptr;

  uint32_t capacity_ = 0;
  std::vector<uint32_t> free_indices_;

  // Bumped on each acquire so stale readbacks from a recycled slot get dropped.
  std::vector<uint32_t> index_generations_;

  std::vector<uint8_t> resolve_batch_pending_;
  // Active indices with resolve_batch_pending_[i] == 1, so flush iterates
  // only the active entries instead of scanning the full capacity.
  std::vector<uint32_t> resolve_batch_indices_;
  std::vector<uint8_t> rov_counter_resolve_batch_pending_;
  std::vector<uint32_t> rov_counter_resolve_batch_indices_;
  // Reusable scratch for coalesced contiguous ranges during flush.
  std::vector<ResolveRange> resolve_batch_ranges_;
};


}  // namespace rex::graphics::d3d12
