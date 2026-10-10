/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once
#include "modkit_timing.hpp"
#include "modkit_texture_barrier.hpp"

namespace reshade::modkit
{
// Buffers referenced by recorded application commands live until device teardown.
// A fence protects CPU mapping, including repeated or cross-queue submissions.
struct texture_snapshot
{
	timing_com<ID3D12Resource> readback;
	timing_com<ID3D12Fence> fence;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
	uint64_t serial = 0, source = 0, bytes = 0, fence_value = 0;
	api::command_queue *requested_queue = nullptr;
	ID3D12CommandQueue *last_queue = nullptr;
	bool recorded = false, captured = false, failed = false;
};
inline std::shared_ptr<texture_snapshot> create_texture_snapshot(api::device *device, api::resource source, uint64_t serial, api::command_queue *queue)
{
	auto native = reinterpret_cast<ID3D12Device *>(device->get_native());
	auto texture = reinterpret_cast<ID3D12Resource *>(source.handle);
	const auto desc = texture->GetDesc();
	if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 ||
		(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) return {};
	D3D12_FEATURE_DATA_FORMAT_INFO format_info {desc.Format, 0};
	if (FAILED(native->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &format_info, sizeof(format_info))) || format_info.PlaneCount != 1) return {};
	auto snapshot = std::make_shared<texture_snapshot>();
	snapshot->serial = serial; snapshot->source = source.handle; snapshot->requested_queue = queue;
	native->GetCopyableFootprints(&desc, 0, 1, 0, &snapshot->footprint, nullptr, nullptr, &snapshot->bytes);
	if (!snapshot->bytes || snapshot->bytes > 64 * 1024 * 1024) return {};
	D3D12_HEAP_PROPERTIES heap {}; heap.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC buffer {}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buffer.Width = snapshot->bytes; buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
	buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (FAILED(native->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&snapshot->readback.ptr))) ||
		FAILED(native->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&snapshot->fence.ptr)))) return {};
	return snapshot;
}
inline void record_texture_snapshot(ID3D12GraphicsCommandList *cmd, const D3D12_RESOURCE_BARRIER &game_barrier, texture_snapshot &snapshot)
{
	auto barrier = game_barrier;
	barrier.Transition.Subresource = 0;
	barrier.Transition.StateBefore = game_barrier.Transition.StateAfter;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	if (barrier.Transition.StateBefore != barrier.Transition.StateAfter) cmd->ResourceBarrier(1, &barrier);
	D3D12_TEXTURE_COPY_LOCATION from {}; from.pResource = game_barrier.Transition.pResource;
	from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION to {}; to.pResource = snapshot.readback.ptr;
	to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = snapshot.footprint;
	cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
	if (barrier.Transition.StateBefore != barrier.Transition.StateAfter)
	{ std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); cmd->ResourceBarrier(1, &barrier); }
	snapshot.recorded = true;
}
}
