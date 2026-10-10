/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once
#include <d3d12.h>
#include <MinHook.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace reshade::modkit
{
// Native query heaps avoid the SDK's per-query fences. Submission readbacks use
// one fence each and are never mapped until the GPU has signaled completion.
constexpr uint32_t timing_page_draws = 256;
constexpr size_t max_timing_pages = 128, max_timing_submissions = 32;
template <class T> struct timing_com
{
	T *ptr = nullptr;
	~timing_com() { if (ptr) ptr->Release(); }
	timing_com() = default;
	timing_com(const timing_com &) = delete;
	timing_com &operator=(const timing_com &) = delete;
};
struct timing_page
{
	timing_com<ID3D12QueryHeap> heap;
	timing_com<ID3D12Fence> last_fence;
	ID3D12CommandQueue *last_queue = nullptr;
	uint32_t used = 0;
	std::shared_ptr<size_t> allocation_count;
	~timing_page() { --*allocation_count; }
};
struct timing_sample { double milliseconds = -1; bool ready = false; };
struct timing_entry
{
	std::shared_ptr<timing_page> page;
	uint32_t query = 0;
	std::shared_ptr<timing_sample> sample;
};
struct timing_submission
{
	timing_com<ID3D12CommandAllocator> allocator;
	timing_com<ID3D12GraphicsCommandList> command;
	timing_com<ID3D12Resource> readback;
	timing_com<ID3D12Fence> fence;
	std::vector<timing_entry> entries;
	std::vector<std::shared_ptr<timing_page>> retained_pages;
	uint64_t frequency = 0;
	ID3D12CommandQueue *queue = nullptr; // Identity only; queue teardown drains its work.
	bool submitted = false;
};
inline std::shared_ptr<timing_page> create_timing_page(api::device *dev, const std::shared_ptr<size_t> &count)
{
	if (*count >= max_timing_pages) return {};
	auto page = std::make_shared<timing_page>();
	page->allocation_count = count; ++*count;
	D3D12_QUERY_HEAP_DESC desc = {}; desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; desc.Count = timing_page_draws * 2;
	auto native = reinterpret_cast<ID3D12Device *>(dev->get_native());
	if (FAILED(native->CreateQueryHeap(&desc, IID_PPV_ARGS(&page->heap.ptr)))) return {};
	return page;
}
using native_execute = void (STDMETHODCALLTYPE *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
static std::atomic<bool> native_timing_enabled {false};
static std::mutex timing_hook_mutex;
static std::recursive_mutex timing_submit_mutex;
static std::unordered_map<void *, native_execute> timing_hooks;
static bool timing_hooks_initialized = false;
static void STDMETHODCALLTYPE timing_execute_hook(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
inline void *execute_target(ID3D12CommandQueue *queue)
{ return (*reinterpret_cast<void ***>(queue))[10]; }
inline native_execute timing_original(ID3D12CommandQueue *queue)
{
	std::lock_guard<std::mutex> lock(timing_hook_mutex);
	const auto found = timing_hooks.find(execute_target(queue));
	return found == timing_hooks.end() ? nullptr : found->second;
}
inline bool install_timing_hook(api::command_queue *queue)
{
	if (queue->get_device()->get_api() != api::device_api::d3d12) return false;
	auto native = reinterpret_cast<ID3D12CommandQueue *>(queue->get_native());
	auto target = execute_target(native);
	std::lock_guard<std::mutex> lock(timing_hook_mutex);
	if (timing_hooks.count(target)) return true;
	if (timing_hooks.size() >= 32) return false;
	if (!timing_hooks_initialized)
	{
		if (MH_Initialize() != MH_OK) return false;
		timing_hooks_initialized = true;
	}
	void *original = nullptr;
	if (MH_CreateHook(target, reinterpret_cast<void *>(timing_execute_hook), &original) != MH_OK) return false;
	timing_hooks.emplace(target, reinterpret_cast<native_execute>(original));
	if (MH_EnableHook(target) == MH_OK) return true;
	timing_hooks.erase(target); MH_RemoveHook(target); return false;
}
inline void remove_timing_hooks()
{
	// Called only during add-on shutdown, with no profiling submissions in flight.
	native_timing_enabled = false;
	if (timing_hooks_initialized) MH_Uninitialize();
	timing_hooks.clear(); timing_hooks_initialized = false;
}
inline bool submit_timing_readback(ID3D12CommandQueue *queue, native_execute original, timing_submission &batch)
{
	if (batch.entries.empty()) return false;
	const auto type = queue->GetDesc().Type;
	if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE) return false;
	ID3D12Device *device = nullptr;
	if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) return false;
	timing_com<ID3D12Device> owner; owner.ptr = device;
	if (FAILED(queue->GetTimestampFrequency(&batch.frequency)) || !batch.frequency) return false;
	if (FAILED(device->CreateCommandAllocator(type, IID_PPV_ARGS(&batch.allocator.ptr)))) return false;
	if (FAILED(device->CreateCommandList(0, type, batch.allocator.ptr, nullptr, IID_PPV_ARGS(&batch.command.ptr)))) return false;
	D3D12_HEAP_PROPERTIES heap = {}; heap.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC desc = {}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = batch.entries.size() * 2 * sizeof(uint64_t); desc.Height = 1; desc.DepthOrArraySize = 1;
	desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&batch.readback.ptr)))) return false;
	if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&batch.fence.ptr)))) return false;
	for (size_t i = 0; i < batch.entries.size(); ++i)
	{
		const auto &entry = batch.entries[i];
		batch.command.ptr->ResolveQueryData(entry.page->heap.ptr, D3D12_QUERY_TYPE_TIMESTAMP, entry.query, 2, batch.readback.ptr, i * 2 * sizeof(uint64_t));
	}
	if (FAILED(batch.command.ptr->Close())) return false;
	ID3D12CommandList *commands[] = {batch.command.ptr};
	original(queue, 1, commands);
	batch.queue = queue; batch.submitted = true;
	// On signal failure keep the resources alive until queue teardown.
	return SUCCEEDED(queue->Signal(batch.fence.ptr, 1));
}
inline bool collect_timing_readback(timing_submission &batch)
{
	const auto completed = batch.fence.ptr ? batch.fence.ptr->GetCompletedValue() : 0;
	if (!batch.submitted || completed == 0) return false;
	if (batch.entries.empty()) return true;
	if (completed == UINT64_MAX)
	{
		for (auto &entry : batch.entries) entry.sample->ready = true;
		return true; // Device removed; results are invalid.
	}
	void *mapped = nullptr;
	const D3D12_RANGE range = {0, batch.entries.size() * 2 * sizeof(uint64_t)};
	if (FAILED(batch.readback.ptr->Map(0, &range, &mapped)))
	{
		for (auto &entry : batch.entries) entry.sample->ready = true;
		return true;
	}
	const auto ticks = static_cast<const uint64_t *>(mapped);
	for (size_t i = 0; i < batch.entries.size(); ++i)
	{
		auto &sample = *batch.entries[i].sample;
		if (ticks[i * 2 + 1] >= ticks[i * 2]) sample.milliseconds = double(ticks[i * 2 + 1] - ticks[i * 2]) * 1000.0 / double(batch.frequency);
		sample.ready = true;
	}
	const D3D12_RANGE empty = {0, 0}; batch.readback.ptr->Unmap(0, &empty);
	return true;
}
}
