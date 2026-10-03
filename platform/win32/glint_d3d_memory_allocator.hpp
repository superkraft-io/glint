#pragma once

/**
 * glint_d3d_memory_allocator.hpp
 * GPU memory allocator for Skia's Direct3D 12 backend that allocates each
 * resource exactly the memory it needs.
 *
 * Without one, Skia uses AMD's D3D12 Memory Allocator with its defaults,
 * which reserves heaps in large blocks (default block size 256 MB, first
 * blocks a fraction of that): a window's private memory jumped by ~60 MB on
 * its first GPU frame while Skia's own resources totalled ~8 MB. A UI
 * renderer allocates few, mostly small resources, so one heap per resource
 * costs little and keeps memory equal to what is in use (Skia's resource
 * cache budget, see gpuResourceCacheBytes(), then bounds it).
 *
 * Each resource is placed at offset 0 of its own heap, which lets
 * createAliasingResource() place a second view of it (Skia uses that for
 * BGRA mipmap generation).
 */

#include <windows.h>
#include <d3d12.h>

#include "include/gpu/ganesh/d3d/GrD3DTypes.h"

class glint_d3d_memory_allocator final : public GrD3DMemoryAllocator
{
public:
  explicit glint_d3d_memory_allocator(ID3D12Device* device) { mDevice.retain(device); }

  gr_cp<ID3D12Resource> createResource(D3D12_HEAP_TYPE heapType, const D3D12_RESOURCE_DESC* desc,
                                       D3D12_RESOURCE_STATES initialState, sk_sp<GrD3DAlloc>* allocation,
                                       const D3D12_CLEAR_VALUE* clearValue) override
  {
    if (!desc) return {};
    const D3D12_RESOURCE_ALLOCATION_INFO info = mDevice->GetResourceAllocationInfo(0, 1, desc);
    if (info.SizeInBytes == UINT64_MAX) return {};

    D3D12_HEAP_DESC heapDesc = {};
    heapDesc.SizeInBytes     = info.SizeInBytes;
    heapDesc.Alignment       = info.Alignment;
    heapDesc.Properties.Type = heapType;
    heapDesc.Flags           = heapFlagsFor(*desc);
    gr_cp<ID3D12Heap> heap;
    if (FAILED(mDevice->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap)))) return {};

    gr_cp<ID3D12Resource> resource;
    if (FAILED(mDevice->CreatePlacedResource(heap.get(), 0, desc, initialState, clearValue, IID_PPV_ARGS(&resource))))
      return {};
    if (allocation) *allocation = sk_sp<GrD3DAlloc>(new heap_alloc(std::move(heap)));
    return resource;
  }

  gr_cp<ID3D12Resource> createAliasingResource(sk_sp<GrD3DAlloc>& allocation, uint64_t localOffset,
                                               const D3D12_RESOURCE_DESC* desc,
                                               D3D12_RESOURCE_STATES initialState,
                                               const D3D12_CLEAR_VALUE* clearValue) override
  {
    auto* alloc = static_cast<heap_alloc*>(allocation.get());
    if (!alloc || !alloc->heap || !desc) return {};
    gr_cp<ID3D12Resource> resource;
    if (FAILED(mDevice->CreatePlacedResource(alloc->heap.get(), localOffset, desc, initialState, clearValue,
                                             IID_PPV_ARGS(&resource))))
      return {};
    return resource;
  }

private:
  // Keeps the heap alive as long as Skia holds the allocation.
  struct heap_alloc final : GrD3DAlloc
  {
    explicit heap_alloc(gr_cp<ID3D12Heap> h) : heap(std::move(h)) {}
    gr_cp<ID3D12Heap> heap;
  };

  // Resource heap tier 1 heaps hold one kind of resource only; these flags
  // are valid on every tier.
  static D3D12_HEAP_FLAGS heapFlagsFor(const D3D12_RESOURCE_DESC& desc)
  {
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
      return D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    if (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
      return D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
    return D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
  }

  gr_cp<ID3D12Device> mDevice;
};
