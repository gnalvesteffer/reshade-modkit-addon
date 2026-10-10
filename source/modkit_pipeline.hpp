/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once
#include "reshade_api.hpp"
#include "modkit_profile.hpp"
#include <cstring>
#include <cstddef>
#include <memory>

namespace reshade::modkit
{
using namespace api;

inline bool is_shader(pipeline_subobject_type type)
{
	switch (type)
	{
	case pipeline_subobject_type::vertex_shader:
	case pipeline_subobject_type::hull_shader:
	case pipeline_subobject_type::domain_shader:
	case pipeline_subobject_type::geometry_shader:
	case pipeline_subobject_type::pixel_shader:
	case pipeline_subobject_type::compute_shader:
	case pipeline_subobject_type::amplification_shader:
	case pipeline_subobject_type::mesh_shader:
	case pipeline_subobject_type::raygen_shader:
	case pipeline_subobject_type::any_hit_shader:
	case pipeline_subobject_type::closest_hit_shader:
	case pipeline_subobject_type::miss_shader:
	case pipeline_subobject_type::intersection_shader:
	case pipeline_subobject_type::callable_shader: return true;
	default: return false;
	}
}
inline size_t subobject_size(pipeline_subobject_type type)
{
	switch (type)
	{
	case pipeline_subobject_type::input_layout: return sizeof(input_element);
	case pipeline_subobject_type::stream_output_state: return sizeof(stream_output_desc);
	case pipeline_subobject_type::blend_state: return sizeof(blend_desc);
	case pipeline_subobject_type::rasterizer_state: return sizeof(rasterizer_desc);
	case pipeline_subobject_type::depth_stencil_state: return sizeof(depth_stencil_desc);
	case pipeline_subobject_type::primitive_topology: return sizeof(primitive_topology);
	case pipeline_subobject_type::depth_stencil_format:
	case pipeline_subobject_type::render_target_formats: return sizeof(format);
	case pipeline_subobject_type::dynamic_pipeline_states: return sizeof(dynamic_state);
	case pipeline_subobject_type::sample_mask:
	case pipeline_subobject_type::sample_count:
	case pipeline_subobject_type::viewport_count:
	case pipeline_subobject_type::max_vertex_count:
	case pipeline_subobject_type::max_payload_size:
	case pipeline_subobject_type::max_attribute_size:
	case pipeline_subobject_type::max_recursion_depth: return sizeof(uint32_t);
	case pipeline_subobject_type::shader_groups: return sizeof(shader_group);
	case pipeline_subobject_type::libraries: return sizeof(pipeline);
	case pipeline_subobject_type::flags: return sizeof(pipeline_flags);
	default: return 0;
	}
}

struct owned_shader
{
	shader_desc desc;
	std::vector<uint8_t> code;
	std::string entry;
	std::vector<uint32_t> ids, values;
	uint64_t hash = 0;
	std::string extension;
	void fix_pointers()
	{
		desc.code = code.data(); desc.code_size = code.size();
		desc.entry_point = entry.empty() ? nullptr : entry.c_str();
		desc.spec_constant_ids = ids.data(); desc.spec_constant_values = values.data();
	}
};
struct owned_subobject
{
	pipeline_subobject_type type;
	uint32_t count;
	// max_align_t storage avoids casting an unaligned byte vector to API structures.
	std::vector<std::max_align_t> storage;
	std::vector<std::string> semantics;
	std::vector<owned_shader> shaders;
	std::vector<shader_desc> shader_descs;
};
struct pipeline_snapshot
{
	pipeline_layout layout = {};
	std::vector<owned_subobject> objects;
	bool rebuildable = true;
	size_t bytes = 0;
	std::vector<pipeline_subobject> materialize()
	{
		std::vector<pipeline_subobject> result;
		for (auto &obj : objects)
		{
			void *data = obj.storage.data();
			if (is_shader(obj.type))
			{
				obj.shader_descs.clear();
				for (auto &shader : obj.shaders) { shader.fix_pointers(); obj.shader_descs.push_back(shader.desc); }
				data = obj.shader_descs.data();
			}
			else if (obj.type == pipeline_subobject_type::input_layout)
			{
				auto elements = static_cast<input_element *>(data);
				for (uint32_t i = 0; i < obj.count; ++i)
					elements[i].semantic = obj.semantics[i].empty() ? nullptr : obj.semantics[i].c_str();
			}
			result.push_back({obj.type, obj.count, data});
		}
		return result;
	}
};

inline pipeline_snapshot capture_pipeline(device_api api, pipeline_layout layout, uint32_t count, const pipeline_subobject *objects)
{
	pipeline_snapshot result;
	result.layout = layout;
	for (uint32_t i = 0; i < count; ++i)
	{
		const auto &source = objects[i];
		owned_subobject object;
		object.type = source.type; object.count = source.count;
		if (source.count && !source.data) { result.rebuildable = false; continue; }
		if (is_shader(source.type))
		{
			// Rebuilding a ray tracing pipeline also requires rebuilding shader binding tables.
			if (source.type >= pipeline_subobject_type::raygen_shader) result.rebuildable = false;
			const auto shaders = static_cast<const shader_desc *>(source.data);
			for (uint32_t j = 0; j < source.count; ++j)
			{
				const auto &desc = shaders[j];
				if (!desc.code || !desc.code_size || desc.code_size > 16 * 1024 * 1024 || result.bytes + desc.code_size > 64 * 1024 * 1024 || desc.spec_constants > 4096)
				{ result.rebuildable = false; continue; }
				owned_shader shader;
				shader.desc = desc;
				shader.code.assign(static_cast<const uint8_t *>(desc.code), static_cast<const uint8_t *>(desc.code) + desc.code_size);
				shader.hash = hash_bytes(desc.code, desc.code_size);
				if (desc.entry_point) shader.entry = desc.entry_point;
				if (desc.spec_constants)
				{
					if (!desc.spec_constant_ids || !desc.spec_constant_values) { result.rebuildable = false; continue; }
					shader.ids.assign(desc.spec_constant_ids, desc.spec_constant_ids + desc.spec_constants);
					shader.values.assign(desc.spec_constant_values, desc.spec_constant_values + desc.spec_constants);
				}
				uint32_t magic = 0;
				if (desc.code_size >= 4) std::memcpy(&magic, desc.code, 4);
				shader.extension = api == device_api::vulkan || magic == 0x07230203 ? ".spv" : api == device_api::opengl ? ".glsl" : ".cso";
				result.bytes += shader.code.size();
				object.shaders.push_back(std::move(shader));
			}
		}
		else
		{
			const size_t element_size = subobject_size(source.type);
			if (!element_size || source.count > 4096) { result.rebuildable = false; continue; }
			const size_t bytes = element_size * source.count;
			object.storage.resize((bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
			if (bytes) std::memcpy(object.storage.data(), source.data, bytes);
			result.bytes += bytes;
			if (source.type == pipeline_subobject_type::input_layout)
			{
				const auto elements = static_cast<const input_element *>(source.data);
				for (uint32_t j = 0; j < source.count; ++j) object.semantics.emplace_back(elements[j].semantic ? elements[j].semantic : "");
			}
			// Library lifetimes and ray tracing group handles are not owned by this snapshot.
			if (source.type == pipeline_subobject_type::libraries || source.type == pipeline_subobject_type::shader_groups) result.rebuildable = false;
		}
		result.objects.push_back(std::move(object));
	}
	return result;
}
}
