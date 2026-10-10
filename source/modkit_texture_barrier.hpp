/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once
#include <d3d12.h>
#include <cstdint>
#include "../vendor/reshade/reshade_api_format.hpp"

namespace reshade::modkit
{
inline bool preview_format(api::format fmt)
{
	using api::format;
	switch (api::format_to_default_typed(fmt, 0))
	{
	case format::r8_unorm: case format::r8_snorm: case format::r8g8_unorm: case format::r8g8_snorm:
	case format::r8g8b8a8_unorm: case format::r8g8b8a8_snorm: case format::r8g8b8a8_unorm_srgb:
	case format::b8g8r8a8_unorm: case format::b8g8r8a8_unorm_srgb: case format::b8g8r8x8_unorm: case format::b8g8r8x8_unorm_srgb:
	case format::r16_float: case format::r16_unorm: case format::r16_snorm:
	case format::r16g16_float: case format::r16g16_unorm: case format::r16g16_snorm:
	case format::r16g16b16a16_float: case format::r16g16b16a16_unorm: case format::r16g16b16a16_snorm:
	case format::r32_float: case format::r32g32_float: case format::r32g32b32_float: case format::r32g32b32a32_float:
	case format::r10g10b10a2_unorm: case format::r11g11b10_float: case format::r9g9b9e5:
	case format::bc1_unorm: case format::bc1_unorm_srgb: case format::bc2_unorm: case format::bc2_unorm_srgb:
	case format::bc3_unorm: case format::bc3_unorm_srgb: case format::bc4_unorm: case format::bc4_snorm:
	case format::bc5_unorm: case format::bc5_snorm: case format::bc6h_ufloat: case format::bc6h_sfloat:
	case format::bc7_unorm: case format::bc7_unorm_srgb: return true;
	default: return false;
	}
}
inline bool snapshot_barrier(const D3D12_RESOURCE_BARRIER &barrier, uint64_t source)
{
	return barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
		barrier.Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE &&
		reinterpret_cast<uintptr_t>(barrier.Transition.pResource) == source &&
		(barrier.Transition.Subresource == 0 || barrier.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
}
}
