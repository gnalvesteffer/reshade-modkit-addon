/* SPDX-License-Identifier: BSD-3-Clause */
#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include "modkit_pipeline.hpp"
#include "modkit_opengl.hpp"
#include "modkit_timing.hpp"
#include <stb_image.h>
#include <stb_image_write.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace reshade::modkit
{
using namespace api;
constexpr size_t max_capture_bytes = 256 * 1024 * 1024;
constexpr size_t max_replacement_bytes = 512 * 1024 * 1024;
constexpr size_t max_draws = 8192, max_tables = 4096, max_pipelines = 1024;
constexpr size_t max_views = 16384;
static std::recursive_mutex mutex;
static thread_local bool internal_call = false;
struct internal_scope
{
	bool previous = internal_call;
	internal_scope() { internal_call = true; }
	~internal_scope() { internal_call = previous; }
};

struct shader_info
{
	uint64_t hash;
	pipeline_subobject_type stage;
	std::string extension, entry;
	std::vector<uint8_t> code;
};
struct pipeline_info
{
	pipeline_snapshot snapshot;
	pipeline replacement = {};
	pipeline gl_rejected = {};
	uint64_t attempted_generation = 0;
};
struct resource_info
{
	resource handle = {}, replacement = {};
	resource_desc desc;
	uint64_t serial = 0;
	std::string id;
	bool content_id = false, disabled = false;
	std::vector<uint8_t> captured; // Packed buffer, or packed base mip without row padding.
	uint64_t attempted_generation = 0;
	resource_view preview = {};
	std::string error;
};
struct view_info { resource res = {}; resource_usage usage; resource_view_desc desc; };
struct descriptor_value
{
	descriptor_type type = descriptor_type::sampler;
	sampler_with_resource_view sampled = {};
	buffer_range buffer;
	uint64_t version = 0;
};
struct layout_param_info
{
	bool table = false, supported = true;
	std::vector<descriptor_range> ranges;
};
struct pipeline_binding { pipeline_stage stages; pipeline handle; };
struct pushed_descriptor
{
	shader_stage stages;
	pipeline_layout layout;
	uint32_t param;
	descriptor_table_update update;
	descriptor_value value;
};
struct table_binding
{
	shader_stage stages;
	pipeline_layout layout;
	uint32_t first;
	std::vector<descriptor_table> tables;
	std::vector<uint32_t> dynamic_offsets;
};
struct vertex_binding { resource buffer = {}; uint64_t offset = 0; uint32_t stride = 0; };
struct draw_record;
struct command_state
{
	std::vector<pipeline_binding> pipelines;
	std::vector<pushed_descriptor> pushed;
	std::vector<table_binding> tables;
	std::map<uint32_t, vertex_binding> vertices;
	resource index = {}; uint64_t index_offset = 0; uint32_t index_size = 0;
	std::vector<resource_view> targets;
	std::vector<std::string> markers;
	std::vector<viewport> viewports;
	std::vector<rect> scissors;
	std::map<dynamic_state, uint32_t> dynamic_states;
	std::vector<draw_record> actions;
	std::shared_ptr<timing_page> query_page;
	std::array<std::vector<uint64_t>, 2> shader_cache;
	std::array<uint64_t, 2> shader_cache_epoch {};
};
struct draw_resource { uint64_t serial; std::string id, role; };
struct draw_record
{
	std::string kind;
	uint32_t count = 0, instances = 0, first = 0;
	int32_t vertex_offset = 0;
	uint64_t command = 0;
	std::vector<uint64_t> shaders;
	std::vector<draw_resource> resources;
	bool modified = false, skipped = false;
	std::string marker;
	std::vector<viewport> viewports;
	std::vector<rect> scissors;
	std::map<dynamic_state, uint32_t> dynamic_states;
	std::shared_ptr<timing_page> query_page;
	uint32_t query_index = 0;
	std::shared_ptr<timing_sample> gpu;
};
struct mapped_buffer { void *data; uint64_t offset, size; map_access access; };
struct mapped_texture { subresource_data data; bool whole; map_access access; };
static size_t action_size(const draw_record &action)
{
	size_t bytes = sizeof(action) + action.kind.size() + action.marker.size() + action.shaders.size() * sizeof(uint64_t) + action.viewports.size() * sizeof(viewport) + action.scissors.size() * sizeof(rect) + action.dynamic_states.size() * 32;
	for (const auto &res : action.resources) bytes += sizeof(res) + res.id.size() + res.role.size();
	return bytes;
}
struct file_info { std::filesystem::file_time_type time; uintmax_t size; uint64_t generation; };
struct descriptor_key_hash
{
	size_t operator()(const std::pair<uint64_t, uint32_t> &key) const
	{ return std::hash<uint64_t>{}(key.first ^ (uint64_t(key.second) * 0x9E3779B97F4A7C15ull)); }
};
struct suppression_comparison
{
	bool active = false, original_suppressed = false;
	uint64_t shader = 0, generation = 0;
	int phase = 0, warmup = 0, waited = 0, samples_per_phase = 120;
	uint32_t seen = 0, skipped = 0;
	std::array<std::vector<double>, 2> samples;
	std::chrono::steady_clock::time_point previous;
	std::string status = "Select a shader to compare its suppression performance.";
};
struct device_state
{
	std::filesystem::path root;
	std::unordered_map<uint64_t, pipeline_info> pipelines;
	std::map<uint64_t, shader_info> shaders;
	std::unordered_map<uint64_t, resource_info> resources;
	std::unordered_map<uint64_t, view_info> views;
	std::unordered_map<uint64_t, std::vector<layout_param_info>> layouts;
	std::unordered_map<uint64_t, uint64_t> layout_versions;
	std::unordered_map<std::pair<uint64_t, uint32_t>, descriptor_value, descriptor_key_hash> descriptors;
	std::map<std::vector<uint64_t>, descriptor_table> shadow_tables;
	std::unordered_map<command_list *, command_state> commands;
	std::unordered_map<uint64_t, command_list *> native_commands;
	std::unordered_map<uint64_t, mapped_buffer> mapped_buffers;
	std::map<std::pair<uint64_t, uint32_t>, mapped_texture> mapped_textures;
	std::vector<pipeline> owned_pipelines;
	std::vector<resource> owned_resources;
	std::vector<resource_view> owned_views;
	std::map<std::pair<uint64_t, uint64_t>, resource_view> replacement_views;
	std::unordered_set<command_queue *> queues;
	std::vector<draw_record> recording, frame;
	std::vector<draw_rule> rules;
	std::unordered_set<uint64_t> suppressed_shaders, disabled_shaders;
	std::map<std::filesystem::path, file_info> files;
	size_t capture_bytes = 0, replacement_bytes = 0;
	size_t action_bytes = 0;
	uint64_t generation = 1, serial = 0, descriptor_version = 0, frame_index = 0;
	uint64_t pipeline_epoch = 1;
	uint64_t total_draws = 0, modified_draws = 0, skipped_draws = 0;
	bool enabled = true, live = false, capture_next = false, recording_frame = false;
	bool watch = true, reload_requested = true, rules_loaded = false;
	bool capture_uploads = false;
	bool detailed_resources = true;
	int selected_draw = -1;
	uint64_t selected_shader = 0, selected_resource = 0;
	char filter[128] = {};
	std::array<char, 65536> editor = {};
	std::filesystem::path editor_path;
	std::string disassembly;
	uint64_t disassembly_shader = 0;
	std::string status = "Ready. Capture a frame to inspect the graphics pipeline.";
	std::chrono::steady_clock::time_point last_poll;
	bool gpu_timing = false, timing_hooked = false, expensive_first = false;
	std::shared_ptr<size_t> timing_page_count = std::make_shared<size_t>(0);
	std::vector<std::shared_ptr<timing_page>> timing_pool, untracked_timing_pages;
	std::vector<std::shared_ptr<timing_submission>> timing_pending;
	uint64_t timing_dropped = 0;
	suppression_comparison comparison;
};
static std::unordered_map<device *, std::unique_ptr<device_state>> devices;
static device_state *find(device *dev)
{
	const auto it = devices.find(dev);
	return it == devices.end() ? nullptr : it->second.get();
}
static bool explicit_api(device *dev) { return dev->get_api() == device_api::d3d12 || dev->get_api() == device_api::vulkan; }
static const char *api_name(device_api api)
{
	switch (api) {
	case device_api::d3d9: return "Direct3D 9"; case device_api::d3d10: return "Direct3D 10";
	case device_api::d3d11: return "Direct3D 11"; case device_api::d3d12: return "Direct3D 12";
	case device_api::opengl: return "OpenGL"; case device_api::vulkan: return "Vulkan"; default: return "Unknown"; }
}
static const char *stage_name(pipeline_subobject_type stage)
{
	switch (stage) {
	case pipeline_subobject_type::vertex_shader: return "VS"; case pipeline_subobject_type::hull_shader: return "HS";
	case pipeline_subobject_type::domain_shader: return "DS"; case pipeline_subobject_type::geometry_shader: return "GS";
	case pipeline_subobject_type::pixel_shader: return "PS"; case pipeline_subobject_type::compute_shader: return "CS";
	case pipeline_subobject_type::mesh_shader: return "MS"; case pipeline_subobject_type::amplification_shader: return "AS";
	default: return "RT"; }
}
static pipeline_stage stage_mask(pipeline_subobject_type stage)
{
	switch (stage) {
	case pipeline_subobject_type::vertex_shader: return pipeline_stage::vertex_shader;
	case pipeline_subobject_type::hull_shader: return pipeline_stage::hull_shader;
	case pipeline_subobject_type::domain_shader: return pipeline_stage::domain_shader;
	case pipeline_subobject_type::geometry_shader: return pipeline_stage::geometry_shader;
	case pipeline_subobject_type::pixel_shader: return pipeline_stage::pixel_shader;
	case pipeline_subobject_type::compute_shader: return pipeline_stage::compute_shader;
	case pipeline_subobject_type::mesh_shader: return pipeline_stage::mesh_shader;
	case pipeline_subobject_type::amplification_shader: return pipeline_stage::amplification_shader;
	default: return pipeline_stage::all_ray_tracing; }
}
static void report(device_state &state, std::string message)
{
	state.status = std::move(message);
	reshade::log::message(reshade::log::level::info, state.status.c_str());
}
static void update_timing_enabled()
{
	bool armed = false;
	for (const auto &device : devices)
	{
		const auto &s = *device.second;
		armed |= s.gpu_timing;
		if (s.timing_hooked) for (const auto &page : s.timing_pool) armed |= page.use_count() > 1;
	}
	native_timing_enabled = armed;
}
static void finish_comparison(device_state &s, const std::string &message)
{
	auto &c = s.comparison;
	if (c.original_suppressed) s.suppressed_shaders.insert(c.shader); else s.suppressed_shaders.erase(c.shader);
	c.active = false; c.status = message;
}
static void start_comparison(device_state &s, uint64_t shader)
{
	if (s.comparison.active || !s.enabled || s.gpu_timing) return;
	const int count = s.comparison.samples_per_phase;
	s.comparison = {}; auto &c = s.comparison;
	c.active = true; c.shader = shader; c.generation = s.generation;
	c.samples_per_phase = count; c.original_suppressed = s.suppressed_shaders.count(shader) != 0;
	s.suppressed_shaders.erase(shader);
	c.status = "Measuring shader enabled. Keep the camera and scene steady.";
}
static void advance_comparison(device_state &s, std::chrono::steady_clock::time_point now)
{
	auto &c = s.comparison; if (!c.active) return;
	const bool suppress = (c.phase & 1) != 0;
	const bool valid = c.seen != 0 && (suppress ? c.skipped == c.seen : c.skipped == 0);
	const double ms = c.previous.time_since_epoch().count() ? std::chrono::duration<double, std::milli>(now - c.previous).count() : 0;
	c.previous = now; c.seen = c.skipped = 0;
	if (!s.enabled || s.generation != c.generation) { finish_comparison(s, "Comparison cancelled because the mod configuration changed."); return; }
	if (++c.waited > 3000) { finish_comparison(s, "Comparison timed out waiting for draws recorded with the requested suppression state."); return; }
	if (!valid || ms <= 0 || ms > 250) { c.warmup = 0; return; }
	if (c.warmup++ < 30) return;
	c.samples[suppress ? 1 : 0].push_back(ms);
	const size_t phase_samples = c.samples[suppress ? 1 : 0].size();
	const size_t target = size_t(c.samples_per_phase) * size_t(c.phase / 2 + 1);
	if (phase_samples < target) return;
	if (++c.phase == 4) { finish_comparison(s, "Comparison complete. The original suppression setting has been restored."); return; }
	c.warmup = c.waited = 0; c.previous = {};
	if (c.phase & 1) s.suppressed_shaders.insert(c.shader); else s.suppressed_shaders.erase(c.shader);
	c.status = c.phase & 1 ? "Measuring shader suppressed. Waiting for updated commands and warmup." : "Measuring shader enabled. Waiting for updated commands and warmup.";
}
static void observe_comparison(device_state &s, const draw_record &action)
{
	auto &c = s.comparison;
	if (c.active && std::find(action.shaders.begin(), action.shaders.end(), c.shader) != action.shaders.end())
	{ ++c.seen; c.skipped += action.skipped ? 1 : 0; }
}
static bool rgba_format(format fmt)
{
	fmt = format_to_default_typed(fmt, 0);
	return fmt == format::r8g8b8a8_unorm || fmt == format::b8g8r8a8_unorm || fmt == format::b8g8r8x8_unorm;
}
static bool bgra_format(format fmt)
{
	fmt = format_to_default_typed(fmt, 0);
	return fmt == format::b8g8r8a8_unorm || fmt == format::b8g8r8x8_unorm;
}
static bool writable_resource(const resource_desc &desc)
{
	return (desc.usage & (resource_usage::render_target | resource_usage::depth_stencil | resource_usage::unordered_access | resource_usage::stream_output)) != resource_usage::undefined;
}
static void capture_data(device_state &state, resource_info &info, const subresource_data &data, bool force = false)
{
	if (!data.data || (!force && !state.capture_uploads)) return;
	std::vector<uint8_t> bytes;
	if (info.desc.type == resource_type::buffer)
	{
		if (info.desc.buffer.size > 64 * 1024 * 1024 || state.capture_bytes - info.captured.size() + info.desc.buffer.size > max_capture_bytes) return;
		bytes.assign(static_cast<const uint8_t *>(data.data), static_cast<const uint8_t *>(data.data) + info.desc.buffer.size);
	}
	else if (info.desc.type == resource_type::texture_2d && info.desc.texture.samples == 1)
	{
		const uint32_t pitch = format_row_pitch(info.desc.texture.format, info.desc.texture.width);
		const uint32_t size = format_slice_pitch(info.desc.texture.format, pitch, info.desc.texture.height);
		if (!pitch || !size || size > 64 * 1024 * 1024 || data.row_pitch < pitch || state.capture_bytes - info.captured.size() + size > max_capture_bytes) return;
		bytes.resize(size);
		for (uint32_t row = 0; row < size / pitch; ++row)
			std::memcpy(bytes.data() + row * pitch, static_cast<const uint8_t *>(data.data) + size_t(row) * data.row_pitch, pitch);
	}
	if (bytes.empty() || state.capture_bytes - info.captured.size() + bytes.size() > max_capture_bytes) return;
	state.capture_bytes -= info.captured.size(); state.capture_bytes += bytes.size();
	info.captured = std::move(bytes);
	uint64_t hash = hash_bytes(info.captured.data(), info.captured.size());
	const uint64_t dimensions[] = { static_cast<uint64_t>(info.desc.type), info.desc.type == resource_type::buffer ? info.desc.buffer.size : info.desc.texture.width,
		info.desc.type == resource_type::buffer ? info.desc.buffer.structured.stride : info.desc.texture.height,
		info.desc.type == resource_type::buffer ? 0 : static_cast<uint64_t>(info.desc.texture.format),
		info.desc.type == resource_type::buffer ? uint64_t(0) : uint64_t(info.desc.texture.depth_or_layers) };
	hash = hash_bytes(dimensions, sizeof(dimensions), hash);
	info.id = hex_id(hash); info.content_id = true; info.attempted_generation = 0;
}

static void on_init_device(device *dev)
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	auto state = std::make_unique<device_state>();
	size_t size = 0; reshade::get_reshade_base_path(nullptr, &size);
	std::string base(size, '\0'); reshade::get_reshade_base_path(base.data(), &size);
	if (const auto nul = base.find('\0'); nul != std::string::npos) base.resize(nul);
	wchar_t executable[32768] = {};
	GetModuleFileNameW(nullptr, executable, 32768);
	state->root = std::filesystem::u8path(base) / "reshade-modkit" / std::filesystem::path(executable).stem();
	std::error_code ec;
	for (const auto *folder : {"shaders", "textures", "buffers", "captures"}) std::filesystem::create_directories(state->root / folder, ec);
	if (ec) state->status = "Cannot create mod directory: " + ec.message();
	if (!std::filesystem::exists(state->root / "rules.ini", ec))
	{
		const char initial[] = "; Mod Kit draw rules. See MODKIT.md for syntax.\n; An empty file leaves all game draws unchanged.\n";
		write_binary(state->root / "rules.ini", initial, sizeof(initial) - 1);
	}
	devices.emplace(dev, std::move(state));
}
static void on_init_queue(command_queue *queue)
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (auto s = find(queue->get_device()))
	{
		s->queues.insert(queue);
		if (s->timing_hooked && queue->get_device()->get_api() == device_api::d3d12 && !install_timing_hook(queue))
		{ s->gpu_timing = false; report(*s, "GPU timing stopped: could not track a new D3D12 queue."); }
	}
}
static void on_destroy_queue(command_queue *queue)
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (auto s = find(queue->get_device()))
	{
		const auto native = reinterpret_cast<ID3D12CommandQueue *>(queue->get_native());
		if (!s->timing_pending.empty() || !s->untracked_timing_pages.empty()) queue->wait_idle();
		s->timing_pending.erase(std::remove_if(s->timing_pending.begin(), s->timing_pending.end(), [&](auto &batch) {
			if (batch->queue != native) return false;
			collect_timing_readback(*batch); return true;
		}), s->timing_pending.end());
		s->queues.erase(queue);
	}
}
static void on_destroy_device(device *dev)
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	auto s = find(dev); if (!s) return;
	internal_scope guard;
	// Device teardown is the only point at which cached command lists cannot be submitted again.
	for (auto &entry : s->shadow_tables) dev->free_descriptor_tables(1, &entry.second);
	for (auto view : s->owned_views) dev->destroy_resource_view(view);
	for (auto pipe : s->owned_pipelines) dev->destroy_pipeline(pipe);
	for (auto res : s->owned_resources) dev->destroy_resource(res);
	devices.erase(dev);
	update_timing_enabled();
}
static void on_init_command(command_list *cmd)
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (auto s = find(cmd->get_device()))
	{
		s->commands.emplace(cmd, command_state{});
		if (cmd->get_device()->get_api() == device_api::d3d12) s->native_commands[cmd->get_native()] = cmd;
	}
}
static void on_reset_command(command_list *cmd)
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (auto s = find(cmd->get_device()))
	{
		for (const auto &action : s->commands[cmd].actions) s->action_bytes -= action_size(action);
		s->commands[cmd] = {};
	}
}
static void on_destroy_command(command_list *cmd)
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (auto s = find(cmd->get_device()))
	{
		if (auto it = s->commands.find(cmd); it != s->commands.end()) for (const auto &action : it->second.actions) s->action_bytes -= action_size(action);
		s->commands.erase(cmd);
		s->native_commands.erase(cmd->get_native());
	}
}
static void on_secondary(command_list *cmd, command_list *secondary)
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	auto s = find(cmd->get_device()); if (!s) return;
	auto &primary = s->commands[cmd]; auto actions = std::move(primary.actions);
	if (auto found = s->commands.find(secondary); found != s->commands.end()) for (const auto &action : found->second.actions)
		if (actions.size() < max_draws && s->action_bytes + action_size(action) <= 64 * 1024 * 1024) { actions.push_back(action); s->action_bytes += action_size(action); }
	// Secondary command buffers may invalidate parent bindings. Preserve the recorded actions.
	primary = {}; primary.actions = std::move(actions);
}
static void on_execute(command_queue *queue, command_list *cmd)
{
	if (internal_call) return;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	auto s = find(queue->get_device()); if (!s || !explicit_api(queue->get_device())) return;
	if (!s->recording_frame && !s->comparison.active) return;
	if (auto found = s->commands.find(cmd); found != s->commands.end())
		for (const auto &action : found->second.actions)
		{
			observe_comparison(*s, action);
			// The native submission hook attaches distinct results for every execution.
			if (s->recording_frame && !(s->timing_hooked && native_timing_enabled && queue->get_device()->get_api() == device_api::d3d12) && s->recording.size() < max_draws)
				s->recording.push_back(action);
		}
}
static void STDMETHODCALLTYPE timing_execute_hook(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists)
{
	auto original = timing_original(queue);
	if (!original) return; // Hooks are removed only at process shutdown.
	if (!native_timing_enabled) { original(queue, count, lists); return; }
	// Keep resolves adjacent to their native submission, including concurrent callers.
	std::lock_guard<std::recursive_mutex> submission_lock(timing_submit_mutex);
	std::lock_guard<std::recursive_mutex> lock(mutex);
	device_state *s = nullptr;
	for (auto &device : devices) for (auto candidate : device.second->queues)
		if (candidate->get_device()->get_api() == device_api::d3d12 && candidate->get_native() == reinterpret_cast<uintptr_t>(queue)) s = device.second.get();
	if (!s || !s->timing_hooked) { original(queue, count, lists); return; }
	auto batch = std::make_shared<timing_submission>();
	std::set<std::pair<timing_page *, uint32_t>> observed;
	std::unordered_set<timing_page *> retained;
	bool duplicate = false;
	for (UINT i = 0; i < count; ++i)
		if (const auto native = s->native_commands.find(reinterpret_cast<uintptr_t>(lists[i])); native != s->native_commands.end())
			if (const auto command = s->commands.find(native->second); command != s->commands.end())
				for (const auto &action : command->second.actions)
				{
					draw_record record; if (s->recording_frame) record = action;
					if (action.query_page)
					{
						if (retained.insert(action.query_page.get()).second) batch->retained_pages.push_back(action.query_page);
						if (s->gpu_timing && s->recording_frame) duplicate |= !observed.emplace(action.query_page.get(), action.query_index).second;
						if (s->gpu_timing && s->recording_frame && batch->entries.size() < max_draws)
						{
							record.gpu = std::make_shared<timing_sample>();
							batch->entries.push_back({action.query_page, action.query_index, record.gpu});
						}
					}
					// Displayed captures own results, not reusable timestamp slots.
					record.query_page.reset();
					if (s->recording_frame && s->recording.size() < max_draws) s->recording.push_back(std::move(record));
				}
	// Shared timestamp slots cannot overlap across queues. Preserve their order
	// when a cached list migrates to another queue (profiling only).
	for (const auto &page : batch->retained_pages)
		if (page->last_fence.ptr && page->last_queue != queue && page->last_fence.ptr->GetCompletedValue() == 0)
			queue->Wait(page->last_fence.ptr, 1);
	original(queue, count, lists);
	if (batch->retained_pages.empty()) return;
	if (duplicate)
	{
		for (auto &entry : batch->entries) entry.sample->ready = true;
		batch->entries.clear(); ++s->timing_dropped;
	}
	if (s->timing_pending.size() >= max_timing_submissions)
	{
		for (auto &entry : batch->entries) entry.sample->ready = true;
		for (auto &page : batch->retained_pages)
			if (std::find(s->untracked_timing_pages.begin(), s->untracked_timing_pages.end(), page) == s->untracked_timing_pages.end()) s->untracked_timing_pages.push_back(page);
		++s->timing_dropped; return;
	}
	bool success = false;
	if (!batch->entries.empty()) success = submit_timing_readback(queue, original, *batch);
	else
	{
		ID3D12Device *device = nullptr;
		if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))))
		{
			if (SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&batch->fence.ptr))))
			{ batch->submitted = true; batch->queue = queue; success = SUCCEEDED(queue->Signal(batch->fence.ptr, 1)); }
			device->Release();
		}
	}
	if (success)
		for (auto &page : batch->retained_pages)
		{
			if (page->last_fence.ptr) page->last_fence.ptr->Release();
			page->last_fence.ptr = batch->fence.ptr; page->last_fence.ptr->AddRef(); page->last_queue = queue;
		}
	else
	{
		for (auto &entry : batch->entries) entry.sample->ready = true;
		++s->timing_dropped;
		// A failed allocation or signal must never release heap slots still in use.
		for (auto &page : batch->retained_pages)
			if (std::find(s->untracked_timing_pages.begin(), s->untracked_timing_pages.end(), page) == s->untracked_timing_pages.end()) s->untracked_timing_pages.push_back(page);
	}
	if (batch->submitted) s->timing_pending.push_back(std::move(batch));
}
static void on_init_pipeline(device *dev, pipeline_layout layout, uint32_t count, const pipeline_subobject *objects, pipeline pipe)
{
	if (internal_call || !pipe.handle) return;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	auto s = find(dev); if (!s) return;
	++s->pipeline_epoch;
	// OpenGL relinks can reuse a program handle without a destroy notification.
	if (auto old = s->pipelines.find(pipe.handle); old != s->pipelines.end())
	{ s->capture_bytes -= old->second.snapshot.bytes; s->pipelines.erase(old); }
	if (s->capture_bytes >= max_capture_bytes) return;
	auto snapshot = capture_pipeline(dev->get_api(), layout, count, objects);
	if (dev->get_api() == device_api::opengl)
		for (const auto &obj : snapshot.objects) for (const auto &shader : obj.shaders)
			if (shader.extension == ".glsl" && std::string(reinterpret_cast<const char *>(shader.code.data()), shader.code.size()).find("subroutine") != std::string::npos)
				snapshot.rebuildable = false; // No subroutine selections in the generic binding API.
	if (dev->get_api() == device_api::d3d11)
		for (const auto &obj : snapshot.objects) for (const auto &shader : obj.shaders)
		{
			ID3D11ShaderReflection *reflection = nullptr;
			if (SUCCEEDED(D3DReflect(shader.code.data(), shader.code.size(), IID_ID3D11ShaderReflection, reinterpret_cast<void **>(&reflection))))
			{
				// ReShade's generic pipeline binding cannot preserve class instances.
				if (reflection->GetNumInterfaceSlots()) snapshot.rebuildable = false;
				reflection->Release();
			}
		}
	if (s->capture_bytes + snapshot.bytes > max_capture_bytes) { report(*s, "Shader capture memory limit reached"); return; }
	for (const auto &obj : snapshot.objects) for (const auto &shader : obj.shaders)
	{
		if (!s->shaders.count(shader.hash) && s->capture_bytes + snapshot.bytes + shader.code.size() <= max_capture_bytes)
		{
			s->shaders.emplace(shader.hash, shader_info{shader.hash, obj.type, shader.extension, shader.entry, shader.code});
			s->capture_bytes += shader.code.size();
		}
	}
	s->capture_bytes += snapshot.bytes;
	s->pipelines[pipe.handle].snapshot = std::move(snapshot);
}
static void on_destroy_pipeline(device *dev, pipeline pipe)
{
	if (internal_call) return;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (auto s = find(dev)) if (auto it = s->pipelines.find(pipe.handle); it != s->pipelines.end())
	{ s->capture_bytes -= it->second.snapshot.bytes; s->pipelines.erase(it); ++s->pipeline_epoch; }
}
static void on_init_resource(device *dev, const resource_desc &desc, const subresource_data *data, resource_usage, resource res)
{
	if (internal_call || !res.handle) return;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	auto s = find(dev); if (!s) return;
	resource_info info; info.handle = res; info.desc = desc; info.serial = ++s->serial;
	// Mutable GL storage can also reuse a resource handle.
	if (auto old = s->resources.find(res.handle); old != s->resources.end())
	{ s->capture_bytes -= old->second.captured.size(); s->resources.erase(old); }
	info.id = "session-" + hex_id(info.serial);
	if (data) capture_data(*s, info, data[0]);
	s->resources[res.handle] = std::move(info);
}
static void on_destroy_resource(device *dev, resource res)
{
	if (internal_call) return;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (auto s = find(dev)) if (auto it = s->resources.find(res.handle); it != s->resources.end())
	{
		s->capture_bytes -= it->second.captured.size(); s->resources.erase(it);
		s->mapped_buffers.erase(res.handle);
		for (auto mapped = s->mapped_textures.begin(); mapped != s->mapped_textures.end();) if (mapped->first.first == res.handle) mapped = s->mapped_textures.erase(mapped); else ++mapped;
	}
}
static void on_init_view(device *dev, resource res, resource_usage usage, const resource_view_desc &desc, resource_view view)
{
	if (internal_call || !view.handle) return;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (auto s = find(dev)) s->views[view.handle] = {res, usage, desc};
}
static void on_destroy_view(device *dev, resource_view view)
{
	if (internal_call) return;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (auto s = find(dev))
	{
		s->views.erase(view.handle);
		for (auto it = s->replacement_views.begin(); it != s->replacement_views.end();) if (it->first.first == view.handle) it = s->replacement_views.erase(it); else ++it;
	}
}
static bool on_update_texture(device *dev, const subresource_data &data, resource dest, uint32_t subresource, const subresource_box *box)
{
	if (internal_call || subresource != 0 || box) return false;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (auto s = find(dev)) if (auto it = s->resources.find(dest.handle); it != s->resources.end()) capture_data(*s, it->second, data);
	return false;
}
static bool on_update_buffer(device *dev, const void *data, resource dest, uint64_t offset, uint64_t size)
{
	if (internal_call || offset || !data) return false;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (auto s = find(dev)) if (auto it = s->resources.find(dest.handle); it != s->resources.end() && (size == UINT64_MAX || size == it->second.desc.buffer.size))
		capture_data(*s, it->second, {const_cast<void *>(data), 0, 0});
	return false;
}
static bool on_update_texture_cmd(command_list *cmd, const subresource_data &data, resource res, uint32_t sub, const subresource_box *box)
{ return on_update_texture(cmd->get_device(), data, res, sub, box); }
static bool on_update_buffer_cmd(command_list *cmd, const void *data, resource res, uint64_t offset, uint64_t size)
{ return on_update_buffer(cmd->get_device(), data, res, offset, size); }

static void on_bind_pipeline(command_list *cmd, pipeline_stage stages, pipeline pipe)
{
	if (internal_call) return;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (auto s = find(cmd->get_device()))
	{
		auto &command = s->commands[cmd];
		if (command.pipelines.size() == 1 && command.pipelines[0].stages == stages && command.pipelines[0].handle == pipe) return;
		command.shader_cache_epoch = {};
		auto &bindings = command.pipelines;
		for (auto &binding : bindings) binding.stages &= ~stages;
		bindings.erase(std::remove_if(bindings.begin(), bindings.end(), [](auto &b) { return b.stages == pipeline_stage{}; }), bindings.end());
		if (pipe.handle) bindings.push_back({stages, pipe});
	}
}
static void on_init_layout(device *dev, uint32_t count, const pipeline_layout_param *params, pipeline_layout layout)
{
	if (internal_call) return;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	auto s = find(dev); if (!s) return;
	auto &out = s->layouts[layout.handle]; out.resize(count);
	s->layout_versions[layout.handle] = ++s->descriptor_version;
	for (uint32_t i = 0; i < count; ++i)
	{
		auto &p = out[i];
		p.table = params[i].type == pipeline_layout_param_type::descriptor_table || params[i].type == pipeline_layout_param_type::descriptor_table_with_flags;
		if (params[i].type == pipeline_layout_param_type::descriptor_table)
			p.ranges.assign(params[i].descriptor_table.ranges, params[i].descriptor_table.ranges + params[i].descriptor_table.count);
		else if (params[i].type == pipeline_layout_param_type::descriptor_table_with_flags)
			for (uint32_t j = 0; j < params[i].descriptor_table_with_flags.count; ++j)
			{
				const auto &r = params[i].descriptor_table_with_flags.ranges[j]; p.ranges.push_back(r);
				if ((r.flags & descriptor_range_flags::partially_bound) != descriptor_range_flags::none || r.static_samplers) p.supported = false;
			}
		for (auto &r : p.ranges) if (r.count == UINT32_MAX || r.count > 255 || r.array_size == 0) p.supported = false;
	}
}
static void on_destroy_layout(device *dev, pipeline_layout layout)
{
	if (internal_call) return;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (auto s = find(dev))
	{
		s->layouts.erase(layout.handle);
		s->layout_versions.erase(layout.handle);
		for (auto &p : s->pipelines) if (p.second.snapshot.layout == layout) p.second.snapshot.rebuildable = false;
	}
}
static descriptor_value read_descriptor(descriptor_type type, const void *data, uint32_t index)
{
	descriptor_value value; value.type = type;
	if (type == descriptor_type::sampler) value.sampled.sampler = static_cast<const sampler *>(data)[index];
	else if (type == descriptor_type::sampler_with_resource_view) value.sampled = static_cast<const sampler_with_resource_view *>(data)[index];
	else if (type == descriptor_type::constant_buffer || type == descriptor_type::shader_storage_buffer || type == descriptor_type::constant_buffer_with_dynamic_offset || type == descriptor_type::shader_storage_buffer_with_dynamic_offset)
		value.buffer = static_cast<const buffer_range *>(data)[index];
	else value.sampled.view = static_cast<const resource_view *>(data)[index];
	return value;
}
static const void *descriptor_pointer(const descriptor_value &value)
{
	if (value.type == descriptor_type::sampler) return &value.sampled.sampler;
	if (value.type == descriptor_type::sampler_with_resource_view) return &value.sampled;
	if (value.type == descriptor_type::constant_buffer || value.type == descriptor_type::shader_storage_buffer || value.type == descriptor_type::constant_buffer_with_dynamic_offset || value.type == descriptor_type::shader_storage_buffer_with_dynamic_offset) return &value.buffer;
	return &value.sampled.view;
}
static bool descriptors_equal(const descriptor_value &a, const descriptor_value &b)
{
	return a.type == b.type && a.sampled.sampler == b.sampled.sampler && a.sampled.view == b.sampled.view && a.buffer.buffer == b.buffer.buffer && a.buffer.offset == b.buffer.offset && a.buffer.size == b.buffer.size;
}
static bool on_update_descriptors(device *dev, uint32_t count, const descriptor_table_update *updates)
{
	if (internal_call) return false;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	auto s = find(dev); if (!s) return false;
	for (uint32_t i = 0; i < count; ++i)
	{
		const auto &u = updates[i]; if (!u.table.handle || !u.descriptors || u.count > 65536) continue;
		descriptor_heap heap = {}; uint32_t offset = 0; dev->get_descriptor_heap_offset(u.table, u.binding, u.array_offset, &heap, &offset);
		if (!heap.handle) continue;
		for (uint32_t j = 0; j < u.count; ++j)
		{
			auto value = read_descriptor(u.type, u.descriptors, j);
			const auto key = std::make_pair(heap.handle, offset + j);
			const auto previous = s->descriptors.find(key);
			value.version = previous != s->descriptors.end() && descriptors_equal(value, previous->second) ? previous->second.version : ++s->descriptor_version;
			s->descriptors[key] = value;
		}
	}
	return false;
}
static bool on_copy_descriptors(device *dev, uint32_t count, const descriptor_table_copy *copies)
{
	if (internal_call) return false;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	auto s = find(dev); if (!s) return false;
	for (uint32_t i = 0; i < count; ++i)
	{
		const auto &c = copies[i]; if (c.count > 65536) continue;
		descriptor_heap src = {}, dst = {}; uint32_t so = 0, to = 0;
		dev->get_descriptor_heap_offset(c.source_table, c.source_binding, c.source_array_offset, &src, &so);
		dev->get_descriptor_heap_offset(c.dest_table, c.dest_binding, c.dest_array_offset, &dst, &to);
		std::vector<std::optional<descriptor_value>> saved(c.count);
		for (uint32_t j = 0; j < c.count; ++j) if (auto it = s->descriptors.find({src.handle, so + j}); it != s->descriptors.end()) saved[j] = it->second;
		for (uint32_t j = 0; j < c.count; ++j)
			if (saved[j])
			{
				const auto key = std::make_pair(dst.handle, to + j);
				const auto previous = s->descriptors.find(key);
				saved[j]->version = previous != s->descriptors.end() && descriptors_equal(*saved[j], previous->second) ? previous->second.version : ++s->descriptor_version;
				s->descriptors[key] = *saved[j];
			}
			else s->descriptors.erase({dst.handle, to + j});
	}
	return false;
}
static void on_push(command_list *cmd, shader_stage stages, pipeline_layout layout, uint32_t param, const descriptor_table_update &update)
{
	if (internal_call || !update.descriptors || update.count > 4096) return;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	auto s = find(cmd->get_device()); if (!s) return;
	auto &out = s->commands[cmd].pushed;
	if (update.count == 1)
		for (auto &old : out)
			if (old.stages == stages && old.layout == layout && old.param == param && old.update.binding == update.binding && old.update.array_offset == update.array_offset && old.update.type == update.type)
			{ old.value = read_descriptor(update.type, update.descriptors, 0); return; }
	for (uint32_t i = 0; i < update.count; ++i)
	{
		pushed_descriptor p{stages, layout, param, update, read_descriptor(update.type, update.descriptors, i)};
		p.update.count = 1; p.update.descriptors = nullptr;
		if (cmd->get_device()->get_api() == device_api::vulkan) p.update.array_offset += i;
		else p.update.binding += i;
		for (auto &old : out) if (old.layout == layout && old.param == param && old.update.binding == p.update.binding && old.update.array_offset == p.update.array_offset && old.update.type == p.update.type) old.stages &= ~stages;
		out.erase(std::remove_if(out.begin(), out.end(), [](const auto &p) { return p.stages == shader_stage{}; }), out.end());
		if (out.size() < 4096) out.push_back(p);
	}
}
static void on_bind_tables(command_list *cmd, shader_stage stages, pipeline_layout layout, uint32_t first, uint32_t count, const descriptor_table *tables, uint32_t offset_count, const uint32_t *offsets)
{
	if (internal_call || count > 128) return;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	auto s = find(cmd->get_device()); if (!s) return;
	auto &out = s->commands[cmd].tables;
	if (count == 1 && offset_count == 0)
		for (auto &old : out)
			if (old.stages == stages && old.layout == layout && old.first == first && old.tables.size() == 1 && old.dynamic_offsets.empty())
			{ old.tables[0] = tables[0]; return; }
	if (!count)
	{
		auto &pushed = s->commands[cmd].pushed;
		pushed.erase(std::remove_if(pushed.begin(), pushed.end(), [&](auto &p) { return (p.stages & stages) != shader_stage{} && p.layout != layout; }), pushed.end());
	}
	// Split binds without dynamic offsets; preserve an entire Vulkan bind when offsets exist.
	for (auto it = out.begin(); it != out.end();)
	{
		if ((it->stages & stages) != shader_stage{} && ((count == 0 && it->layout != layout) || (count && first < it->first + it->tables.size() && it->first < first + count))) it = out.erase(it);
		else ++it;
	}
	if (!count) return;
	if (offset_count)
	{
		const auto params = s->layouts.find(layout.handle);
		uint32_t consumed = 0;
		std::vector<table_binding> split;
		if (params != s->layouts.end() && first + count <= params->second.size())
			for (uint32_t i = 0; i < count; ++i)
			{
				uint32_t dynamic_count = 0;
				for (const auto &r : params->second[first + i].ranges)
					if (r.type == descriptor_type::constant_buffer_with_dynamic_offset || r.type == descriptor_type::shader_storage_buffer_with_dynamic_offset) dynamic_count += r.count;
				if (dynamic_count > offset_count - consumed) { split.clear(); break; }
				split.push_back({stages, layout, first + i, {tables[i]}, {offsets + consumed, offsets + consumed + dynamic_count}});
				consumed += dynamic_count;
			}
		if (consumed == offset_count && split.size() == count) out.insert(out.end(), split.begin(), split.end());
		else out.push_back({stages, layout, first, {tables, tables + count}, {offsets, offsets + offset_count}});
	}
	else for (uint32_t i = 0; i < count; ++i) out.push_back({stages, layout, first + i, {tables[i]}, {}});
}
static void on_vertices(command_list *cmd, uint32_t first, uint32_t count, const resource *buffers, const uint64_t *offsets, const uint32_t *strides)
{
	if (internal_call || count > 128) return;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (auto s = find(cmd->get_device())) for (uint32_t i = 0; i < count; ++i)
		s->commands[cmd].vertices[first + i] = {buffers[i], offsets ? offsets[i] : 0, strides ? strides[i] : 0};
}
static void on_index(command_list *cmd, resource buffer, uint64_t offset, uint32_t size)
{
	if (internal_call) return;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (auto s = find(cmd->get_device())) { auto &c = s->commands[cmd]; c.index = buffer; c.index_offset = offset; c.index_size = size; }
}
static void on_targets(command_list *cmd, uint32_t count, const resource_view *views, resource_view depth)
{
	if (internal_call || count > 32) return;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (auto s = find(cmd->get_device())) { auto &targets = s->commands[cmd].targets; targets.clear(); if (count) targets.assign(views, views + count); if (depth.handle) targets.push_back(depth); }
}
static bool on_render_pass(command_list *cmd, uint32_t count, const render_pass_render_target_desc *targets, const render_pass_depth_stencil_desc *depth, render_pass_flags)
{
	std::vector<resource_view> views; for (uint32_t i = 0; i < count; ++i) views.push_back(targets[i].view);
	on_targets(cmd, count, views.data(), depth ? depth->view : resource_view{}); return false;
}

static bool file_present(device_state &s, const std::filesystem::path &path) { return s.files.count(path) != 0; }
static void poll_files(device_state &s)
{
	std::map<std::filesystem::path, file_info> latest;
	std::error_code ec;
	bool changed = s.reload_requested;
	for (const auto *folder : {"shaders", "textures", "buffers"})
		for (std::filesystem::directory_iterator it(s.root / folder, ec), end; !ec && it != end; it.increment(ec))
		{
			const auto &entry = *it;
			if (!entry.is_regular_file(ec)) continue;
			const auto path = entry.path();
			const auto time = entry.last_write_time(ec); if (ec) break;
			const auto size = entry.file_size(ec); if (ec) break;
			if (size > 64 * 1024 * 1024) continue;
			auto previous = s.files.find(path);
			const bool different = previous == s.files.end() || previous->second.time != time || previous->second.size != size || s.reload_requested;
			latest.emplace(path, file_info{time, size, different ? ++s.generation : previous->second.generation});
			changed |= different;
		}
	if (ec) { report(s, "Could not scan replacement files: " + ec.message()); s.reload_requested = false; return; }
	changed |= latest.size() != s.files.size();
	s.files.swap(latest);
	const auto rules_path = s.root / "rules.ini";
	const auto rules_time = std::filesystem::last_write_time(rules_path, ec);
	static std::map<std::filesystem::path, std::filesystem::file_time_type> rule_times;
	if (!ec && (!s.rules_loaded || s.reload_requested || rule_times[rules_path] != rules_time))
	{
		std::vector<draw_rule> rules; std::string error;
		if (read_rules(rules_path, rules, error)) { s.rules = std::move(rules); s.rules_loaded = true; rule_times[rules_path] = rules_time; changed = true; }
		else report(s, error + "; previous draw rules remain active");
	}
	if (changed) ++s.generation;
	s.reload_requested = false;
}
static pipeline replacement_pipeline(device *dev, device_state &s, pipeline_info &info)
{
	if (!info.snapshot.rebuildable) return {};
	std::vector<uint64_t> versions;
	bool replacement_found = false;
	for (const auto &obj : info.snapshot.objects) for (const auto &shader : obj.shaders)
	{
		const auto path = s.root / "shaders" / (hex_id(shader.hash) + shader.extension);
		auto file = s.files.find(path);
		const bool active = file != s.files.end() && !s.disabled_shaders.count(shader.hash);
		versions.push_back(active ? file->second.generation : 0); replacement_found |= active;
	}
	if (!replacement_found) { info.replacement = {}; return {}; }
	const uint64_t signature = hash_bytes(versions.data(), versions.size() * sizeof(uint64_t));
	if (info.attempted_generation == signature) return info.replacement;
	info.attempted_generation = signature;
	if (s.owned_pipelines.size() >= max_pipelines) { report(s, "Pipeline version limit reached. Restart the game to reclaim retained versions."); return info.replacement; }
	auto snapshot = info.snapshot;
	for (auto &obj : snapshot.objects) for (auto &shader : obj.shaders)
	{
		const auto path = s.root / "shaders" / (hex_id(shader.hash) + shader.extension);
		if (!file_present(s, path) || s.disabled_shaders.count(shader.hash)) continue;
		std::vector<uint8_t> code;
		if (!read_binary(path, code)) { report(s, "Could not load shader " + path.filename().u8string()); return info.replacement; }
		if (shader.extension == ".spv" && (code.size() % 4 || code.size() < 20)) { report(s, "Invalid SPIR-V size for " + hex_id(shader.hash)); return info.replacement; }
		shader.code = std::move(code);
	}
	auto subobjects = snapshot.materialize();
	pipeline pipe = {};
	internal_scope guard;
	if (!dev->create_pipeline(snapshot.layout, static_cast<uint32_t>(subobjects.size()), subobjects.data(), &pipe))
	{ report(s, "Shader compilation/pipeline creation failed; retaining the last working pipeline"); return info.replacement; }
	info.replacement = pipe; s.owned_pipelines.push_back(pipe); return pipe;
}

static resource replacement_resource(device *dev, device_state &s, resource_info &info)
{
	if (info.disabled || writable_resource(info.desc)) return {};
	const bool texture = info.desc.type == resource_type::texture_2d;
	if (!texture && info.desc.type != resource_type::buffer) return {};
	if (texture && (info.desc.texture.samples != 1 || info.desc.texture.depth_or_layers != 1 || info.desc.texture.levels == 0 || info.desc.texture.levels > 16)) return {};
	auto path = s.root / (texture ? "textures" : "buffers") / (info.id + ".bin");
	const auto png_path = s.root / "textures" / (info.id + ".png");
	const bool png = texture && file_present(s, png_path);
	if (png) path = png_path;
	auto file = s.files.find(path);
	if (file == s.files.end()) { info.replacement = {}; return {}; }
	if (info.attempted_generation == file->second.generation) return info.replacement;
	info.attempted_generation = file->second.generation; info.error.clear();
	std::vector<uint8_t> bytes;
	if (!read_binary(path, bytes)) { info.error = "Could not read replacement file"; return info.replacement; }
	std::vector<std::vector<uint8_t>> levels;
	std::vector<subresource_data> initial;
	size_t total_bytes = 0;
	if (png)
	{
		if (!rgba_format(info.desc.texture.format)) { info.error = "PNG requires an RGBA8/BGRA8 texture; use packed native .bin for other formats"; return info.replacement; }
		int width = 0, height = 0, channels = 0;
		if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels) || width != static_cast<int>(info.desc.texture.width) || height != static_cast<int>(info.desc.texture.height) || uint64_t(width) * height * 4 > 64 * 1024 * 1024)
		{ info.error = "PNG must have the original texture dimensions (maximum 64 MiB decoded)"; return info.replacement; }
		auto pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4);
		if (!pixels) { info.error = "PNG decoding failed"; return info.replacement; }
		levels.emplace_back(pixels, pixels + size_t(width) * height * 4); stbi_image_free(pixels);
		if (bgra_format(info.desc.texture.format)) for (size_t i = 0; i < levels[0].size(); i += 4) std::swap(levels[0][i], levels[0][i + 2]);
		for (uint32_t mip = 1; mip < info.desc.texture.levels; ++mip)
		{
			const uint32_t old_w = std::max(1u, info.desc.texture.width >> (mip - 1)), old_h = std::max(1u, info.desc.texture.height >> (mip - 1));
			const uint32_t w = std::max(1u, old_w / 2), h = std::max(1u, old_h / 2);
			std::vector<uint8_t> level(size_t(w) * h * 4);
			const auto &previous = levels.back();
			for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) for (uint32_t c = 0; c < 4; ++c)
			{
				uint32_t sum = 0;
				for (uint32_t dy = 0; dy < 2; ++dy) for (uint32_t dx = 0; dx < 2; ++dx)
					sum += previous[(size_t(std::min(y * 2 + dy, old_h - 1)) * old_w + std::min(x * 2 + dx, old_w - 1)) * 4 + c];
				level[(size_t(y) * w + x) * 4 + c] = static_cast<uint8_t>(sum / 4);
			}
			levels.push_back(std::move(level));
		}
	}
	if (texture)
	{
		for (uint32_t mip = 0; mip < info.desc.texture.levels; ++mip)
		{
			const uint32_t pitch = format_row_pitch(info.desc.texture.format, std::max(1u, info.desc.texture.width >> mip));
			const uint32_t size = format_slice_pitch(info.desc.texture.format, pitch, std::max(1u, info.desc.texture.height >> mip));
			if (!size || (!png && total_bytes + size > bytes.size())) { info.error = "Native .bin must contain all tightly packed mip levels"; return info.replacement; }
			initial.push_back({png ? levels[mip].data() : bytes.data() + total_bytes, pitch, size}); total_bytes += size;
		}
		if (!png && total_bytes != bytes.size()) { info.error = "Native texture replacement size does not match the complete mip chain"; return info.replacement; }
	}
	else
	{
		if (bytes.size() != info.desc.buffer.size) { info.error = "Buffer replacement must have the original size"; return info.replacement; }
		initial.push_back({bytes.data(), 0, 0}); total_bytes = bytes.size();
	}
	if (s.replacement_bytes + total_bytes > max_replacement_bytes) { info.error = "Replacement memory limit reached; restart the game to reclaim retained versions"; return info.replacement; }
	resource_desc desc = info.desc;
	desc.heap = memory_heap::default_; desc.flags = resource_flags::none;
	desc.usage = texture ? resource_usage::shader_resource | resource_usage::copy_dest : desc.usage | resource_usage::copy_dest;
	const resource_usage state = texture ? resource_usage::shader_resource : desc.usage & ~(resource_usage::copy_dest | resource_usage::copy_source);
	resource res = {};
	internal_scope guard;
	if (!dev->create_resource(desc, initial.data(), state, &res)) { info.error = "GPU resource creation failed"; return info.replacement; }
	info.replacement = res; info.preview = {};
	s.owned_resources.push_back(res); s.replacement_bytes += total_bytes;
	return res;
}
static resource replace_buffer(device *dev, device_state &s, resource original)
{
	if (auto it = s.resources.find(original.handle); it != s.resources.end())
		if (const auto replacement = replacement_resource(dev, s, it->second); replacement.handle) return replacement;
	return original;
}
static resource_view replace_view(device *dev, device_state &s, resource_view original)
{
	const auto view = s.views.find(original.handle);
	if (view == s.views.end() || (view->second.usage & resource_usage::shader_resource) == resource_usage::undefined) return original;
	const auto res = s.resources.find(view->second.res.handle);
	if (res == s.resources.end()) return original;
	const auto replacement = replacement_resource(dev, s, res->second);
	if (!replacement.handle) return original;
	const auto key = std::make_pair(original.handle, replacement.handle);
	if (auto existing = s.replacement_views.find(key); existing != s.replacement_views.end()) return existing->second;
	if (s.owned_views.size() >= max_views) { res->second.error = "Retained view limit reached; restart the game to reclaim versions"; return original; }
	resource_view result = {};
	internal_scope guard;
	if (!dev->create_resource_view(replacement, view->second.usage, view->second.desc, &result))
	{ res->second.error = "Replacement view creation failed"; return original; }
	s.replacement_views.emplace(key, result); s.owned_views.push_back(result); return result;
}
static descriptor_value replace_descriptor(device *dev, device_state &s, descriptor_value value)
{
	if (value.type == descriptor_type::shader_resource_view || value.type == descriptor_type::buffer_shader_resource_view || value.type == descriptor_type::sampler_with_resource_view)
		value.sampled.view = replace_view(dev, s, value.sampled.view);
	else if (value.type == descriptor_type::constant_buffer || value.type == descriptor_type::constant_buffer_with_dynamic_offset)
		value.buffer.buffer = replace_buffer(dev, s, value.buffer.buffer);
	return value;
}
static bool descriptor_changed(const descriptor_value &a, const descriptor_value &b)
{ return a.sampled.view != b.sampled.view || a.buffer.buffer != b.buffer.buffer; }

struct table_patch { descriptor_table_update update; descriptor_value value; };
static descriptor_table shadow_table(device *dev, device_state &s, pipeline_layout layout, uint32_t param, descriptor_table original, bool &changed, bool force = false)
{
	changed = false;
	const auto found = s.layouts.find(layout.handle);
	if (found == s.layouts.end() || param >= found->second.size()) return {};
	const auto &p = found->second[param]; if (!p.table || !p.supported) return {};
	std::vector<uint64_t> key{layout.handle, s.layout_versions[layout.handle], param, original.handle};
	std::vector<table_patch> patches;
	std::vector<descriptor_table_copy> copies;
	uint32_t total = 0;
	for (auto &range : p.ranges)
	{
		total += range.count; if (total > 255) return {};
		if (!range.count) continue;
		copies.push_back({original, range.binding, 0, {}, range.binding, 0, range.count});
		for (uint32_t i = 0; i < range.count; ++i)
		{
			const uint32_t binding = range.binding + (i < range.array_size ? 0 : 1 + i - range.array_size);
			const uint32_t array_offset = i < range.array_size ? i : 0;
			descriptor_heap heap = {}; uint32_t offset = 0;
			dev->get_descriptor_heap_offset(original, binding, array_offset, &heap, &offset);
			const auto stored = s.descriptors.find({heap.handle, offset});
			// Avoid copying descriptors that were not observed or may be uninitialized.
			if (stored == s.descriptors.end()) return {};
			auto replacement = replace_descriptor(dev, s, stored->second);
			key.push_back(stored->second.version); key.push_back(replacement.sampled.view.handle); key.push_back(replacement.buffer.buffer.handle);
			if (descriptor_changed(replacement, stored->second))
			{
				changed = true;
				patches.push_back({{{}, binding, array_offset, 1, replacement.type, nullptr}, replacement});
			}
		}
	}
	if (!changed && !force) return original;
	if (const auto cached = s.shadow_tables.find(key); cached != s.shadow_tables.end()) return cached->second;
	if (s.shadow_tables.size() >= max_tables) { report(s, "Descriptor snapshot limit reached; restart the game to reclaim retained tables"); return {}; }
	descriptor_table table = {};
	internal_scope guard;
	if (!dev->allocate_descriptor_tables(1, layout, param, &table)) return {};
	for (auto &copy : copies) copy.dest_table = table;
	dev->copy_descriptor_tables(static_cast<uint32_t>(copies.size()), copies.data());
	for (auto &patch : patches)
	{ patch.update.table = table; patch.update.descriptors = descriptor_pointer(patch.value); dev->update_descriptor_tables(1, &patch.update); }
	s.shadow_tables.emplace(std::move(key), table); return table;
}

static const std::vector<uint64_t> &bound_shaders(device_state &s, command_state &command, bool compute)
{
	const size_t index = compute ? 1 : 0;
	auto &result = command.shader_cache[index];
	if (command.shader_cache_epoch[index] == s.pipeline_epoch) return result;
	command.shader_cache_epoch[index] = s.pipeline_epoch; result.clear();
	const auto relevant = compute ? pipeline_stage::compute_shader : pipeline_stage::all_graphics;
	for (auto &binding : command.pipelines)
		if (auto pipe = s.pipelines.find(binding.handle.handle); pipe != s.pipelines.end())
			for (auto &obj : pipe->second.snapshot.objects)
				if ((stage_mask(obj.type) & binding.stages & relevant) != pipeline_stage{})
					for (auto &shader : obj.shaders) if (std::find(result.begin(), result.end(), shader.hash) == result.end()) result.push_back(shader.hash);
	return result;
}
static void add_resource(device_state &s, draw_record &record, resource res, std::string role)
{
	if (auto it = s.resources.find(res.handle); it != s.resources.end())
		if (record.resources.size() < 128) record.resources.push_back({it->second.serial, it->second.id, std::move(role)});
}
static void add_descriptor_resource(device_state &s, draw_record &record, const descriptor_value &value, const std::string &role)
{
	if (value.buffer.buffer.handle) add_resource(s, record, value.buffer.buffer, role);
	if (auto view = s.views.find(value.sampled.view.handle); view != s.views.end()) add_resource(s, record, view->second.res, role);
}
static draw_record make_draw_record(device *dev, device_state &s, command_state &command, const char *kind, uint32_t count, uint32_t instances, uint32_t first, int32_t vertex_offset, bool compute)
{
	draw_record record; record.kind = kind; record.count = count; record.instances = instances; record.first = first; record.vertex_offset = vertex_offset;
	record.shaders = bound_shaders(s, command, compute);
	// Comparisons and idle timing retain only action and shader metadata.
	if (!s.recording_frame && !s.capture_next && !s.live) return record;
	for (auto &label : command.markers) { if (!record.marker.empty()) record.marker += " / "; record.marker += label; }
	record.viewports = command.viewports; record.scissors = command.scissors; record.dynamic_states = command.dynamic_states;
	if (!s.detailed_resources) return record;
	for (auto &binding : command.vertices) add_resource(s, record, binding.second.buffer, "Vertex " + std::to_string(binding.first));
	add_resource(s, record, command.index, "Index");
	for (auto target : command.targets) if (auto view = s.views.find(target.handle); view != s.views.end()) add_resource(s, record, view->second.res, "Output/depth");
	const auto relevant = compute ? shader_stage::all_compute : shader_stage::all_graphics;
	for (auto &p : command.pushed) if ((p.stages & relevant) != shader_stage{})
		add_descriptor_resource(s, record, p.value, "Binding " + std::to_string(p.update.binding));
	for (auto &b : command.tables) if ((b.stages & relevant) != shader_stage{})
		for (uint32_t t = 0; t < b.tables.size(); ++t)
		{
			const auto layout = s.layouts.find(b.layout.handle); if (layout == s.layouts.end() || b.first + t >= layout->second.size()) continue;
			for (auto &range : layout->second[b.first + t].ranges)
			{
				if (range.count > 4096) continue;
				for (uint32_t i = 0; i < range.count && record.resources.size() < 128; ++i)
				{
					const uint32_t binding = range.binding + (i < range.array_size ? 0 : 1 + i - range.array_size);
					descriptor_heap heap = {}; uint32_t offset = 0;
					dev->get_descriptor_heap_offset(b.tables[t], binding, i < range.array_size ? i : 0, &heap, &offset);
					if (auto d = s.descriptors.find({heap.handle, offset}); d != s.descriptors.end()) add_descriptor_resource(s, record, d->second, "Table " + std::to_string(b.first + t) + ": " + std::to_string(binding));
				}
			}
		}
	return record;
}

// Apply an override only for this action and restore the application's state immediately after it.
struct draw_override
{
	command_list *cmd;
	device_state &s;
	command_state &state;
	std::vector<pipeline_binding> pipes;
	std::vector<pushed_descriptor> pushes;
	std::vector<table_binding> tables;
	std::vector<std::pair<uint32_t, vertex_binding>> vertices;
	std::optional<gl_program_state> gl_state;
	bool index = false, changed = false;
	draw_override(command_list *cmd, device_state &s, command_state &state) : cmd(cmd), s(s), state(state) {}
	bool prepare(bool compute)
	{
		auto dev = cmd->get_device();
		const auto relevant = compute ? pipeline_stage::compute_shader : pipeline_stage::all_graphics;
		const auto shader_relevant = compute ? shader_stage::all_compute : shader_stage::all_graphics;
		internal_scope guard;
		for (auto &binding : state.pipelines) if ((binding.stages & relevant) != pipeline_stage{})
			if (auto p = s.pipelines.find(binding.handle.handle); p != s.pipelines.end())
				if (auto replacement = replacement_pipeline(dev, s, p->second); replacement.handle)
				{
					if (p->second.gl_rejected == replacement) continue;
					if (dev->get_api() == device_api::opengl && !gl_state) gl_state.emplace();
					cmd->bind_pipeline(binding.stages & relevant, replacement);
					if (gl_state)
					{
						gl_state->restore_alpha();
						if (!copy_gl_program(gl_state->program))
						{
							cmd->bind_pipeline(binding.stages & relevant, binding.handle); gl_state->restore_alpha();
							p->second.gl_rejected = replacement;
							report(s, "GL shader replacement has an unsupported uniform interface; original program retained");
							continue;
						}
					}
					pipes.push_back({binding.stages & relevant, binding.handle}); changed = true;
				}
		for (auto &p : state.pushed) if ((p.stages & shader_relevant) != shader_stage{})
		{
			auto replacement = replace_descriptor(dev, s, p.value);
			if (!descriptor_changed(replacement, p.value)) continue;
			auto update = p.update; update.descriptors = descriptor_pointer(replacement);
			cmd->push_descriptors(p.stages, p.layout, p.param, update); pushes.push_back(p); changed = true;
		}
		if (!compute)
		{
			for (auto &v : state.vertices)
			{
				auto replacement = replace_buffer(dev, s, v.second.buffer); if (replacement == v.second.buffer) continue;
				cmd->bind_vertex_buffers(v.first, 1, &replacement, &v.second.offset, &v.second.stride); vertices.push_back(v); changed = true;
			}
			auto replacement = replace_buffer(dev, s, state.index);
			if (replacement != state.index) { cmd->bind_index_buffer(replacement, state.index_offset, state.index_size); index = changed = true; }
		}
		// D3D12 descriptor heaps are shared by every table. Clone and rebind all tables together.
		bool tables_changed = false, tables_valid = true;
		std::vector<table_binding> candidates;
		for (auto &b : state.tables)
		{
			if ((b.stages & shader_relevant) == shader_stage{}) continue;
			auto candidate = b;
			for (uint32_t i = 0; i < b.tables.size(); ++i)
			{
				bool changed = false;
				candidate.tables[i] = shadow_table(dev, s, b.layout, b.first + i, b.tables[i], changed);
				tables_changed |= changed; tables_valid &= candidate.tables[i].handle != 0;
			}
			candidates.push_back(std::move(candidate));
		}
		if (tables_valid && tables_changed)
		{
			if (dev->get_api() == device_api::d3d12)
				for (auto &b : candidates) for (uint32_t i = 0; i < b.tables.size(); ++i)
				{
					const auto &original = *std::find_if(state.tables.begin(), state.tables.end(), [&](auto &entry) { return entry.layout == b.layout && entry.first == b.first && entry.stages == b.stages; });
					bool ignored = false;
					b.tables[i] = shadow_table(dev, s, b.layout, b.first + i, original.tables[i], ignored, true);
					tables_valid &= b.tables[i].handle != 0;
				}
			if (!tables_valid) return changed;
			// Changing either D3D12 heap invalidates all root tables. A second pass binds
			// every table after both heaps have settled, including view and sampler tables.
			for (unsigned pass = 0; pass < (dev->get_api() == device_api::d3d12 ? 2u : 1u); ++pass)
				for (auto &b : candidates) cmd->bind_descriptor_tables2(b.stages, b.layout, b.first, static_cast<uint32_t>(b.tables.size()), b.tables.data(), static_cast<uint32_t>(b.dynamic_offsets.size()), b.dynamic_offsets.data());
			tables = state.tables; changed = true;
		}
		return changed;
	}
	~draw_override()
	{
		internal_scope guard;
		for (unsigned pass = 0; pass < (cmd->get_device()->get_api() == device_api::d3d12 ? 2u : 1u); ++pass)
			for (auto &b : tables) cmd->bind_descriptor_tables2(b.stages, b.layout, b.first, static_cast<uint32_t>(b.tables.size()), b.tables.data(), static_cast<uint32_t>(b.dynamic_offsets.size()), b.dynamic_offsets.data());
		for (auto &v : vertices) cmd->bind_vertex_buffers(v.first, 1, &v.second.buffer, &v.second.offset, &v.second.stride);
		if (index) cmd->bind_index_buffer(state.index, state.index_offset, state.index_size);
		for (auto &p : pushes) { auto update = p.update; update.descriptors = descriptor_pointer(p.value); cmd->push_descriptors(p.stages, p.layout, p.param, update); }
		for (auto &p : pipes) cmd->bind_pipeline(p.stages, p.handle);
		if (gl_state) gl_state->restore_alpha();
	}
};

template <typename Replay>
static bool intercept(command_list *cmd, const char *kind, uint32_t count, uint32_t instances, uint32_t first, int32_t vertex_offset, bool compute, Replay replay, bool replayable = true)
{
	if (internal_call) return false;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	auto s = find(cmd->get_device()); if (!s) return false;
	auto &command = s->commands[cmd];
	++s->total_draws;
	const bool deferred = explicit_api(cmd->get_device());
	const bool capture = s->recording_frame || s->capture_next || s->live || s->gpu_timing || s->comparison.active;
	bool recording = capture && (deferred ? command.actions.size() < max_draws && s->action_bytes < 64 * 1024 * 1024 : s->recording.size() < max_draws);
	const bool have_mods = s->enabled && (!s->suppressed_shaders.empty() || !s->rules.empty() || !s->files.empty());
	if (!recording && !have_mods) return false;
	draw_record record;
	if (recording) { record = make_draw_record(cmd->get_device(), *s, command, kind, count, instances, first, vertex_offset, compute); record.command = reinterpret_cast<uintptr_t>(cmd); }
	// Do not emit timestamps unless their metadata can be retained for submission tracking.
	if (recording && deferred && s->action_bytes + action_size(record) > 64 * 1024 * 1024) recording = false;
	const auto &shaders = recording ? record.shaders : bound_shaders(*s, command, compute);
	bool skip = false, modify = false, timed = false;
	uint32_t new_count = count, new_instances = instances;
	if (s->enabled)
	{
		for (auto shader : shaders) skip |= s->suppressed_shaders.count(shader) != 0;
		for (auto &rule : s->rules)
		{
			if (rule.kind != "any" && rule.kind != kind) continue;
			if (rule.shader && std::find(shaders.begin(), shaders.end(), *rule.shader) == shaders.end()) continue;
			if (rule.count && *rule.count != count) continue;
			if (rule.instances && *rule.instances != instances) continue;
			skip |= rule.skip;
			if (std::string(kind) != "indirect")
			{
				if (rule.replace_count) new_count = *rule.replace_count;
				if (rule.replace_instances) new_instances = *rule.replace_instances;
			}
		}
		modify = new_count != count || new_instances != instances;
	}
	if (!skip && replayable)
	{
		draw_override overrides(cmd, *s, command);
		// Suppression and draw rules need no resource replacement scan.
		if (s->enabled && !s->files.empty()) modify |= overrides.prepare(compute);
		if (s->gpu_timing && recording && cmd->get_device()->get_api() == device_api::d3d12)
		{
			const auto type = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native())->GetType();
			if (type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE)
			{
				if (!command.query_page || command.query_page->used == timing_page_draws)
				{
					command.query_page.reset();
					for (const auto &page : s->timing_pool) if (page.use_count() == 1)
					{ command.query_page = page; page->used = 0; break; }
					if (!command.query_page)
						if (auto page = create_timing_page(cmd->get_device(), s->timing_page_count))
						{ s->timing_pool.push_back(page); command.query_page = std::move(page); }
				}
				if (command.query_page)
				{
					record.query_page = command.query_page; record.query_index = command.query_page->used++ * 2;
					internal_scope guard;
					cmd->end_query({reinterpret_cast<uintptr_t>(record.query_page->heap.ptr)}, query_type::timestamp, record.query_index);
					replay(new_count, new_instances);
					cmd->end_query({reinterpret_cast<uintptr_t>(record.query_page->heap.ptr)}, query_type::timestamp, record.query_index + 1);
					timed = true;
				}
				else ++s->timing_dropped;
			}
		}
		if (modify && !timed) { internal_scope guard; replay(new_count, new_instances); }
	}
	if (skip) ++s->skipped_draws;
	if (modify) ++s->modified_draws;
	if (recording)
	{
		record.skipped = skip; record.modified = modify;
		if (!deferred) observe_comparison(*s, record);
		if (deferred)
		{
			const auto bytes = action_size(record);
			if (s->action_bytes + bytes <= 64 * 1024 * 1024) { s->action_bytes += bytes; command.actions.push_back(std::move(record)); }
		}
		else if (s->recording_frame) s->recording.push_back(std::move(record));
	}
	return skip || modify || timed;
}
static bool on_draw(command_list *cmd, uint32_t count, uint32_t instances, uint32_t first, uint32_t first_instance)
{ return intercept(cmd, "draw", count, instances, first, 0, false, [&](uint32_t c, uint32_t i) { cmd->draw(c, i, first, first_instance); }); }
static bool on_draw_indexed(command_list *cmd, uint32_t count, uint32_t instances, uint32_t first, int32_t offset, uint32_t first_instance)
{ return intercept(cmd, "indexed", count, instances, first, offset, false, [&](uint32_t c, uint32_t i) { cmd->draw_indexed(c, i, first, offset, first_instance); }); }
static bool on_dispatch(command_list *cmd, uint32_t x, uint32_t y, uint32_t z)
{ return intercept(cmd, "dispatch", x, y, z, 0, true, [&](uint32_t a, uint32_t b) { cmd->dispatch(a, b, z); }); }
static bool on_mesh(command_list *cmd, uint32_t x, uint32_t y, uint32_t z)
{ return intercept(cmd, "mesh", x, y, z, 0, false, [&](uint32_t a, uint32_t b) { cmd->dispatch_mesh(a, b, z); }); }
static bool on_indirect(command_list *cmd, indirect_command type, resource buffer, uint64_t offset, uint32_t count, uint32_t stride)
{ return intercept(cmd, "indirect", count, stride, 0, 0, type == indirect_command::dispatch, [&](uint32_t, uint32_t) { cmd->draw_or_dispatch_indirect(type, buffer, offset, count, stride); }, type != indirect_command::unknown); }
static void on_present(command_queue *queue, swapchain *, const rect *, const rect *, uint32_t, const rect *)
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	auto s = find(queue->get_device()); if (!s) return;
	if (s->recording_frame) { s->frame.swap(s->recording); s->recording.clear(); ++s->frame_index; }
	s->recording_frame = s->live || s->capture_next; s->capture_next = false;
	const auto now = std::chrono::steady_clock::now();
	advance_comparison(*s, now);
	s->timing_pending.erase(std::remove_if(s->timing_pending.begin(), s->timing_pending.end(), [](auto &batch) { return collect_timing_readback(*batch); }), s->timing_pending.end());
	if (s->timing_hooked) update_timing_enabled();
	if (s->reload_requested || (s->watch && now - s->last_poll > std::chrono::milliseconds(500))) { poll_files(*s); s->last_poll = now; }
}

static void on_map_buffer(device *dev, resource res, uint64_t offset, uint64_t size, map_access access, void **data)
{
	if (internal_call || !data || !*data || access == map_access::read_only) return;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (auto s = find(dev); s && s->capture_uploads) s->mapped_buffers[res.handle] = {*data, offset, size, access};
}
static void on_unmap_buffer(device *dev, resource res)
{
	if (internal_call) return;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	auto s = find(dev); if (!s) return;
	const auto mapped = s->mapped_buffers.find(res.handle), end = s->mapped_buffers.end(); if (mapped == end) return;
	const auto info = s->resources.find(res.handle);
	if (info != s->resources.end() && mapped->second.offset == 0 && (mapped->second.size == UINT64_MAX || mapped->second.size >= info->second.desc.buffer.size))
		capture_data(*s, info->second, {mapped->second.data, 0, 0});
	s->mapped_buffers.erase(mapped);
}
static void on_map_texture(device *dev, resource res, uint32_t sub, const subresource_box *box, map_access access, subresource_data *data)
{
	if (internal_call || !data || !data->data || access == map_access::read_only) return;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (auto s = find(dev); s && s->capture_uploads) s->mapped_textures[{res.handle, sub}] = {*data, box == nullptr, access};
}
static void on_unmap_texture(device *dev, resource res, uint32_t sub)
{
	if (internal_call) return;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	auto s = find(dev); if (!s) return;
	const auto mapped = s->mapped_textures.find({res.handle, sub}); if (mapped == s->mapped_textures.end()) return;
	if (auto info = s->resources.find(res.handle); info != s->resources.end() && sub == 0 && mapped->second.whole) capture_data(*s, info->second, mapped->second.data);
	s->mapped_textures.erase(mapped);
}
static bool on_buffer_to_texture(command_list *cmd, resource source, uint64_t offset, uint32_t row_length, uint32_t slice_height, resource dest, uint32_t sub, const subresource_box *box)
{
	if (internal_call || sub != 0 || box) return false;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	auto s = find(cmd->get_device()); if (!s) return false;
	const auto from = s->resources.find(source.handle), to = s->resources.find(dest.handle);
	if (from != s->resources.end() && to != s->resources.end() && to->second.desc.type == resource_type::texture_2d)
	{
		const auto &desc = to->second.desc;
		const uint32_t pitch = format_row_pitch(desc.texture.format, row_length ? row_length : desc.texture.width);
		const uint32_t size = format_slice_pitch(desc.texture.format, pitch, slice_height ? slice_height : desc.texture.height);
		if (offset <= from->second.captured.size() && size <= from->second.captured.size() - offset)
			capture_data(*s, to->second, {from->second.captured.data() + offset, pitch, size});
	}
	return false;
}
static void on_viewports(command_list *cmd, uint32_t first, uint32_t count, const viewport *values)
{
	if (internal_call || first + count > 32) return;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (auto s = find(cmd->get_device())) { auto &out = s->commands[cmd].viewports; out.resize(std::max(out.size(), size_t(first + count))); std::copy_n(values, count, out.begin() + first); }
}
static void on_scissors(command_list *cmd, uint32_t first, uint32_t count, const rect *values)
{
	if (internal_call || first + count > 32) return;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (auto s = find(cmd->get_device())) { auto &out = s->commands[cmd].scissors; out.resize(std::max(out.size(), size_t(first + count))); std::copy_n(values, count, out.begin() + first); }
}
static void on_dynamic_states(command_list *cmd, uint32_t count, const dynamic_state *states, const uint32_t *values)
{
	if (internal_call || count > 128) return;
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (auto s = find(cmd->get_device())) for (uint32_t i = 0; i < count; ++i) s->commands[cmd].dynamic_states[states[i]] = values[i];
}

static resource_info *resource_by_serial(device_state &s, uint64_t serial)
{ for (auto &res : s.resources) if (res.second.serial == serial) return &res.second; return nullptr; }
static bool capture_gpu(effect_runtime *runtime, device_state &s, resource_info &info)
{
	auto dev = runtime->get_device();
	if (explicit_api(dev)) { report(s, "GPU readback for explicit APIs needs per-subresource state and queue ownership tracking. CPU upload snapshots are available."); return false; }
	if (info.desc.type != resource_type::buffer && (info.desc.type != resource_type::texture_2d || info.desc.texture.samples != 1)) return false;
	resource_desc staging = info.desc;
	staging.heap = memory_heap::readback; staging.usage = resource_usage::copy_dest; staging.flags = resource_flags::none;
	if (staging.type == resource_type::texture_2d) { staging.texture.levels = 1; staging.texture.depth_or_layers = 1; }
	resource temporary = {};
	internal_scope guard;
	if (!dev->create_resource(staging, nullptr, resource_usage::copy_dest, &temporary)) { report(s, "Could not create staging resource"); return false; }
	auto queue = runtime->get_command_queue(); auto cmd = queue->get_immediate_command_list();
	if (!cmd) { dev->destroy_resource(temporary); return false; }
	if (staging.type == resource_type::buffer) cmd->copy_buffer_region(info.handle, 0, temporary, 0, staging.buffer.size);
	else cmd->copy_texture_region(info.handle, 0, nullptr, temporary, 0, nullptr);
	queue->flush_immediate_command_list(); queue->wait_idle();
	bool success = false;
	if (staging.type == resource_type::buffer)
	{
		void *data = nullptr;
		if (dev->map_buffer_region(temporary, 0, staging.buffer.size, map_access::read_only, &data))
		{ capture_data(s, info, {data, 0, 0}, true); dev->unmap_buffer_region(temporary); success = true; }
	}
	else
	{
		subresource_data data;
		if (dev->map_texture_region(temporary, 0, nullptr, map_access::read_only, &data))
		{ capture_data(s, info, data, true); dev->unmap_texture_region(temporary, 0); success = true; }
	}
	dev->destroy_resource(temporary);
	if (!success) report(s, "GPU snapshot mapping failed");
	return success;
}
static resource_view make_preview(device *dev, device_state &s, resource_info &info)
{
	if (info.preview.handle) return info.preview;
	if (s.owned_views.size() >= max_views) return {};
	if (info.desc.type != resource_type::texture_2d || info.desc.texture.samples != 1) return {};
	resource res = info.replacement;
	internal_scope guard;
	if (!res.handle)
	{
		if (info.captured.empty() || s.replacement_bytes + info.captured.size() > max_replacement_bytes) return {};
		resource_desc desc(info.desc.texture.width, info.desc.texture.height, 1, 1, format_to_default_typed(info.desc.texture.format), 1, memory_heap::default_, resource_usage::shader_resource | resource_usage::copy_dest);
		const uint32_t pitch = format_row_pitch(desc.texture.format, desc.texture.width);
		subresource_data initial{info.captured.data(), pitch, static_cast<uint32_t>(info.captured.size())};
		if (!dev->create_resource(desc, &initial, resource_usage::shader_resource, &res)) return {};
		s.owned_resources.push_back(res); s.replacement_bytes += info.captured.size();
	}
	resource_view_desc desc(format_to_default_typed(info.desc.texture.format));
	if (!dev->create_resource_view(res, resource_usage::shader_resource, desc, &info.preview)) return {};
	s.owned_views.push_back(info.preview); return info.preview;
}
static void png_write(void *context, void *data, int size)
{ static_cast<std::ofstream *>(context)->write(static_cast<const char *>(data), size); }
static void export_resource(device_state &s, resource_info &info)
{
	if (info.captured.empty()) { report(s, "No CPU snapshot is available for this resource"); return; }
	const bool texture = info.desc.type == resource_type::texture_2d;
	auto path = s.root / "captures" / (info.id + (texture ? ".mip0.bin" : ".bin"));
	bool success = write_binary(path, info.captured.data(), info.captured.size());
	if (texture && rgba_format(info.desc.texture.format))
	{
		auto rgba = info.captured;
		if (bgra_format(info.desc.texture.format)) for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
		path = s.root / "captures" / (info.id + ".png"); std::ofstream output(path, std::ios::binary);
		success &= output && stbi_write_png_to_func(png_write, &output, static_cast<int>(info.desc.texture.width), static_cast<int>(info.desc.texture.height), 4, rgba.data(), static_cast<int>(info.desc.texture.width * 4)) != 0;
		success &= static_cast<bool>(output);
	}
	report(s, success ? "Exported " + path.u8string() : "Resource export failed");
}
static void seed_resource(device_state &s, resource_info &info)
{
	if (info.desc.type == resource_type::buffer)
	{
		const auto path = s.root / "buffers" / (info.id + ".bin");
		std::error_code ec; if (std::filesystem::exists(path, ec)) { report(s, "A buffer replacement file already exists"); return; }
		if (!info.captured.empty() && write_binary(path, info.captured.data(), info.captured.size())) { s.reload_requested = true; report(s, "Created buffer replacement " + path.u8string()); }
		else report(s, "No complete buffer snapshot is available");
	}
	else if (rgba_format(info.desc.texture.format) && !info.captured.empty())
	{
		auto rgba = info.captured;
		if (bgra_format(info.desc.texture.format)) for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
		const auto path = s.root / "textures" / (info.id + ".png");
		std::error_code ec; if (std::filesystem::exists(path, ec)) { report(s, "A texture replacement file already exists"); return; }
		std::ofstream output(path, std::ios::binary);
		if (output && stbi_write_png_to_func(png_write, &output, static_cast<int>(info.desc.texture.width), static_cast<int>(info.desc.texture.height), 4, rgba.data(), static_cast<int>(info.desc.texture.width * 4))) { s.reload_requested = true; report(s, "Created texture replacement " + path.u8string()); }
	}
	else report(s, "Export the native data and supply a complete mip chain, or supply an RGBA8/BGRA8 PNG");
}
static bool matches_filter(device_state &s, const std::string &value) { return !s.filter[0] || value.find(s.filter) != std::string::npos; }
static void load_editor(device_state &s, const std::filesystem::path &path, const std::string &fallback)
{
	s.editor_path = path; std::vector<uint8_t> bytes;
	const std::string content = read_binary(path, bytes) ? std::string(bytes.begin(), bytes.end()) : fallback;
	s.editor.fill(0);
	std::memcpy(s.editor.data(), content.data(), std::min(content.size(), s.editor.size() - 1));
	if (content.size() >= s.editor.size()) report(s, "Editor text limit is 64 KiB; use an external editor for larger files");
}
static std::string hlsl_target(const shader_info &shader)
{
	ID3D11ShaderReflection *reflection = nullptr;
	if (FAILED(D3DReflect(shader.code.data(), shader.code.size(), IID_ID3D11ShaderReflection, reinterpret_cast<void **>(&reflection)))) return {};
	D3D11_SHADER_DESC desc = {}; const auto result = reflection->GetDesc(&desc); reflection->Release();
	if (FAILED(result)) return {};
	std::string stage = stage_name(shader.stage); for (auto &c : stage) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return stage + "_" + std::to_string((desc.Version >> 4) & 15) + "_" + std::to_string(desc.Version & 15);
}
static void compile_editor(device_state &s, const shader_info &shader)
{
	const auto target = hlsl_target(shader);
	if (target.empty()) { report(s, "This shader needs an external compiler (for example DXC for Shader Model 6)"); return; }
	ID3DBlob *code = nullptr, *errors = nullptr;
	const auto result = D3DCompile(s.editor.data(), std::strlen(s.editor.data()), s.editor_path.u8string().c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, shader.entry.empty() ? "main" : shader.entry.c_str(), target.c_str(), D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
	std::string diagnostic;
	if (errors) { diagnostic.assign(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize()); errors->Release(); }
	if (FAILED(result)) { report(s, diagnostic.empty() ? "HLSL compilation failed" : diagnostic); if (code) code->Release(); return; }
	const auto path = s.root / "shaders" / (hex_id(shader.hash) + ".cso");
	const bool success = write_binary(path, code->GetBufferPointer(), code->GetBufferSize()); code->Release();
	if (success) { s.reload_requested = true; report(s, "Compiled " + target + "; replacement will apply to new draw recordings"); }
	else report(s, "Could not save compiled shader");
}
static void show_shader(device_state &s, shader_info &shader)
{
	ImGui::Text("%s %s (%zu bytes)", stage_name(shader.stage), hex_id(shader.hash).c_str(), shader.code.size());
	ImGui::BeginDisabled(s.comparison.active);
	bool enabled = s.disabled_shaders.count(shader.hash) == 0;
	if (ImGui::Checkbox("Enable replacement##shader", &enabled)) { if (enabled) s.disabled_shaders.erase(shader.hash); else s.disabled_shaders.insert(shader.hash); }
	ImGui::SameLine(); bool skip = s.suppressed_shaders.count(shader.hash) != 0;
	if (ImGui::Checkbox("Suppress draws using this shader", &skip)) { if (skip) s.suppressed_shaders.insert(shader.hash); else s.suppressed_shaders.erase(shader.hash); }
	ImGui::EndDisabled();
	ImGui::BeginDisabled(s.comparison.active || s.gpu_timing || !s.enabled);
	if (ImGui::Button("Compare suppression performance")) { s.selected_shader = shader.hash; start_comparison(s, shader.hash); }
	ImGui::EndDisabled();
	const auto path = s.root / "shaders" / (hex_id(shader.hash) + shader.extension);
	ImGui::TextWrapped("Replacement: %s", path.u8string().c_str());
	if (ImGui::Button("Export original shader")) report(s, write_binary(s.root / "captures" / path.filename(), shader.code.data(), shader.code.size()) ? "Shader exported to captures/" : "Shader export failed");
	ImGui::SameLine();
	if (ImGui::Button("Create replacement"))
	{
		if (file_present(s, path)) report(s, "A replacement file already exists");
		else if (write_binary(path, shader.code.data(), shader.code.size())) { s.reload_requested = true; report(s, "Created " + path.u8string()); }
	}
	ImGui::SameLine();
	if (ImGui::Button("Edit source"))
	{
		if (shader.extension == ".glsl") load_editor(s, path, std::string(shader.code.begin(), shader.code.end()));
		else load_editor(s, s.root / "shaders" / (hex_id(shader.hash) + ".hlsl"), "// Replacement for " + hex_id(shader.hash) + "\n// Supply HLSL with the same entry point and input/output/resource interface.\n");
	}
	if (ImGui::CollapsingHeader("Shader disassembly/source"))
	{
		if (s.disassembly_shader != shader.hash)
		{
			s.disassembly_shader = shader.hash; s.disassembly.clear();
			if (shader.extension == ".glsl") s.disassembly.assign(shader.code.begin(), shader.code.end());
			else if (shader.extension == ".cso")
			{
				ID3DBlob *assembly = nullptr;
				if (SUCCEEDED(D3DDisassemble(shader.code.data(), shader.code.size(), 0, nullptr, &assembly))) { s.disassembly.assign(static_cast<const char *>(assembly->GetBufferPointer()), assembly->GetBufferSize()); assembly->Release(); }
			}
			if (s.disassembly.empty()) s.disassembly = "Use an external SPIR-V/DXIL disassembler for this shader format.";
		}
		ImGui::BeginChild("assembly", ImVec2(0, 230), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
		ImGui::TextUnformatted(s.disassembly.c_str()); ImGui::EndChild();
	}
}
static void show_resource(effect_runtime *runtime, device_state &s, resource_info &info)
{
	auto dev = runtime->get_device();
	ImGui::TextUnformatted(info.id.c_str());
	if (!info.content_id) ImGui::TextDisabled("Session ID: this resource has no CPU content snapshot yet.");
	if (info.desc.type == resource_type::buffer) ImGui::Text("Buffer: %llu bytes, stride %u", static_cast<unsigned long long>(info.desc.buffer.size), info.desc.buffer.structured.stride);
	else ImGui::Text("Texture: %u x %u, layers %u, mips %u, format %u, samples %u", info.desc.texture.width, info.desc.texture.height, info.desc.texture.depth_or_layers, info.desc.texture.levels, static_cast<uint32_t>(info.desc.texture.format), info.desc.texture.samples);
	ImGui::Text("Handle: %s  Usage: 0x%08X", hex_id(info.handle.handle).c_str(), static_cast<uint32_t>(info.desc.usage));
	if (writable_resource(info.desc)) ImGui::TextDisabled("Writable resources are inspectable; automatic replacement is disabled.");
	else
	{
		bool enabled = !info.disabled;
		if (ImGui::Checkbox("Enable replacement##resource", &enabled)) info.disabled = !enabled;
		if (enabled && s.enabled) replacement_resource(dev, s, info);
	}
	if (ImGui::Button("Snapshot GPU contents")) { if (capture_gpu(runtime, s, info)) info.preview = {}; }
	ImGui::SameLine(); if (ImGui::Button("Export data")) export_resource(s, info);
	ImGui::SameLine(); if (ImGui::Button("Create replacement")) seed_resource(s, info);
	ImGui::TextWrapped("Replacement: %s/%s.%s", (s.root / (info.desc.type == resource_type::buffer ? "buffers" : "textures")).u8string().c_str(), info.id.c_str(), info.desc.type == resource_type::buffer ? "bin" : "png or bin");
	if (!info.error.empty()) ImGui::TextWrapped("%s", info.error.c_str());
	if (info.desc.type == resource_type::texture_2d)
	{
		if (const auto view = make_preview(dev, s, info); view.handle)
		{
			const float width = std::min(ImGui::GetContentRegionAvail().x, 512.0f);
			ImGui::Image(ImTextureRef(view.handle), ImVec2(width, width * info.desc.texture.height / std::max(1u, info.desc.texture.width)));
			ImGui::TextDisabled("Preview of a retained snapshot/replacement. Take a new snapshot to refresh.");
		}
		else ImGui::TextDisabled("No preview is available. Capture CPU uploads or snapshot GPU contents.");
	}
	else if (!info.captured.empty())
	{
		ImGui::BeginChild("buffer_hex", ImVec2(0, 180), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
		for (size_t offset = 0; offset < std::min(size_t(4096), info.captured.size()); offset += 16)
		{
			std::string row = hex_id(offset).substr(8) + "  ";
			for (size_t i = offset; i < std::min(offset + 16, info.captured.size()); ++i) row += hex_id(info.captured[i]).substr(14) + " ";
			ImGui::TextUnformatted(row.c_str());
		}
		ImGui::EndChild();
	}
}
static void export_frame(device_state &s)
{
	const auto path = s.root / "captures" / ("frame-" + std::to_string(s.frame_index) + ".tsv");
	std::ofstream file(path); file << "event\tkind\tcount\tinstances\tfirst\tvertex_offset\tcommand\tskipped\tmodified\tshaders\tresources\tmarker\tgpu_ms\ttiming_status\n";
	for (size_t i = 0; i < s.frame.size(); ++i)
	{
		const auto &r = s.frame[i];
		file << i + 1 << '\t' << r.kind << '\t' << r.count << '\t' << r.instances << '\t' << r.first << '\t' << r.vertex_offset << '\t' << hex_id(r.command) << '\t' << r.skipped << '\t' << r.modified << '\t';
		for (auto shader : r.shaders) file << hex_id(shader) << ' ';
		file << '\t'; for (auto &res : r.resources) file << res.id << ' ';
		std::string marker = r.marker; std::replace(marker.begin(), marker.end(), '\t', ' '); std::replace(marker.begin(), marker.end(), '\n', ' ');
		file << '\t' << marker << '\t';
		if (r.gpu && r.gpu->ready && r.gpu->milliseconds >= 0) file << r.gpu->milliseconds;
		file << '\t' << (r.skipped ? "suppressed" : !r.gpu ? "unavailable" : !r.gpu->ready ? "pending" : r.gpu->milliseconds < 0 ? "unavailable" : "ready") << '\n';
	}
	report(s, file ? "Exported " + path.u8string() : "Frame export failed");
}
static double gpu_ms(const draw_record &record)
{ return record.gpu && record.gpu->ready ? record.gpu->milliseconds : -1; }
static void show_gpu_time(const draw_record &record)
{
	if (record.skipped) ImGui::TextDisabled("skipped");
	else if (gpu_ms(record) >= 0) ImGui::Text("%.4f", gpu_ms(record));
	else ImGui::TextDisabled(record.gpu && !record.gpu->ready ? "pending" : "--");
}
static void show_profiler(effect_runtime *runtime, device_state &s)
{
	const bool supported = runtime->get_device()->get_api() == device_api::d3d12;
	ImGui::BeginDisabled(!supported || s.comparison.active);
	if (ImGui::Checkbox("GPU timing (adds profiling overhead)", &s.gpu_timing))
	{
		if (s.gpu_timing)
		{
			bool success = !s.queues.empty();
			for (auto queue : s.queues) success &= install_timing_hook(queue);
			if (!success) { s.gpu_timing = false; report(s, "Could not install D3D12 submission tracking; GPU timing stays disabled."); }
			else { s.timing_hooked = true; s.capture_next = true; report(s, "GPU timing enabled. Newly recorded direct draws and dispatches will be timed."); }
		}
		update_timing_enabled();
	}
	ImGui::EndDisabled();
	if (!supported) ImGui::TextDisabled("GPU timing is currently available on Direct3D 12.");
	ImGui::TextWrapped("Timing is off by default. Capture a frame after enabling it. Results arrive asynchronously without waiting for the GPU. Cached commands retain timestamps until the game records them again after you disable timing.");
	if (s.gpu_timing && ImGui::Button("Capture timed frame")) { s.capture_next = true; s.live = false; }
	ImGui::Text("Timestamp pages: %zu / %zu | Pending readbacks: %zu / %zu | Dropped: %llu", s.timing_pool.size(), max_timing_pages, s.timing_pending.size(), max_timing_submissions, static_cast<unsigned long long>(s.timing_dropped));
	struct shader_cost { uint64_t shader = 0; uint32_t count = 0; double total = 0, maximum = 0; };
	std::map<uint64_t, shader_cost> costs;
	for (const auto &record : s.frame) if (gpu_ms(record) >= 0)
		for (const auto shader : record.shaders)
		{ auto &cost = costs[shader]; cost.shader = shader; ++cost.count; cost.total += gpu_ms(record); cost.maximum = std::max(cost.maximum, gpu_ms(record)); }
	std::vector<shader_cost> sorted; for (const auto &cost : costs) sorted.push_back(cost.second);
	std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.total > b.total; });
	ImGui::TextWrapped("Shader costs include the whole draw using that shader. Stages share the same draw cost. Timestamp intervals are estimates; GPU overlap and profiling overhead affect them.");
	if (ImGui::BeginTable("shader_costs", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 200)))
	{
		ImGui::TableSetupColumn("Shader"); ImGui::TableSetupColumn("Timed draws"); ImGui::TableSetupColumn("Total ms"); ImGui::TableSetupColumn("Average ms"); ImGui::TableSetupColumn("Max ms"); ImGui::TableHeadersRow();
		for (const auto &cost : sorted)
		{
			const auto label = hex_id(cost.shader); if (!matches_filter(s, label)) continue;
			ImGui::TableNextRow(); ImGui::TableNextColumn();
			if (ImGui::Selectable(label.c_str(), s.selected_shader == cost.shader, ImGuiSelectableFlags_SpanAllColumns)) s.selected_shader = cost.shader;
			ImGui::TableNextColumn(); ImGui::Text("%u", cost.count);
			ImGui::TableNextColumn(); ImGui::Text("%.4f", cost.total);
			ImGui::TableNextColumn(); ImGui::Text("%.4f", cost.total / cost.count);
			ImGui::TableNextColumn(); ImGui::Text("%.4f", cost.maximum);
		}
		ImGui::EndTable();
	}
	ImGui::SeparatorText("Suppression comparison");
	auto &c = s.comparison;
	ImGui::Text("Selected shader: %s", hex_id(s.selected_shader).c_str());
	ImGui::BeginDisabled(c.active);
	ImGui::SliderInt("Samples per phase", &c.samples_per_phase, 60, 600);
	ImGui::EndDisabled();
	ImGui::BeginDisabled(c.active || s.gpu_timing || !s.enabled || !s.selected_shader);
	if (ImGui::Button("Measure enabled / suppressed")) start_comparison(s, s.selected_shader);
	ImGui::EndDisabled(); ImGui::SameLine();
	if (c.active && ImGui::Button("Cancel comparison")) finish_comparison(s, "Comparison cancelled. The original suppression setting has been restored.");
	ImGui::TextWrapped("%s", c.status.c_str());
	if (c.active) ImGui::Text("Phase %d / 4 | Warmup %d / 30 | Samples %zu / %d", c.phase + 1, std::min(c.warmup, 30), c.samples[c.phase & 1].size() - size_t(c.samples_per_phase) * size_t(c.phase / 2), c.samples_per_phase);
	std::array<double, 2> mean {};
	for (size_t i = 0; i < 2; ++i) if (!c.samples[i].empty())
	{
		for (auto ms : c.samples[i]) mean[i] += ms;
		mean[i] /= c.samples[i].size();
		double variance = 0; for (auto ms : c.samples[i]) variance += (ms - mean[i]) * (ms - mean[i]);
		ImGui::Text("%s: %.3f ms average, %.3f ms standard deviation (%zu samples)", i ? "Suppressed" : "Enabled", mean[i], std::sqrt(variance / c.samples[i].size()), c.samples[i].size());
	}
	if (!c.active && mean[0] && mean[1]) ImGui::Text("Frame time reduction: %.3f ms (%.1f%%)", mean[0] - mean[1], (mean[0] - mean[1]) * 100.0 / mean[0]);
	ImGui::TextWrapped("Measures CPU-observed present intervals, including frame caps and VSync. Keep the scene and camera steady. Four alternating phases discard warmup and wait for the actual recorded suppression state. Disable GPU timing first. Other rules can prevent the target shader from drawing and invalidate the comparison.");
}
static void show_draws(effect_runtime *runtime, device_state &s)
{
	if (ImGui::Button("Capture next frame")) { s.capture_next = true; s.live = false; }
	ImGui::SameLine(); ImGui::Checkbox("Live capture", &s.live);
	ImGui::SameLine(); if (ImGui::Button("Freeze")) { s.live = false; s.recording_frame = false; }
	ImGui::SameLine(); if (ImGui::Button("Export frame")) export_frame(s);
	ImGui::Checkbox("Sort expensive draws first", &s.expensive_first);
	ImGui::SameLine(); ImGui::Checkbox("Capture resource bindings", &s.detailed_resources);
	ImGui::Text("Frame %llu: %zu actions%s", static_cast<unsigned long long>(s.frame_index), s.frame.size(), s.frame.size() == max_draws ? " (capture limit reached)" : "");
	const float list_width = std::max(240.0f, ImGui::GetContentRegionAvail().x * 0.44f);
	ImGui::BeginChild("events", ImVec2(list_width, 450), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
	if (ImGui::BeginTable("draw_list", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY))
	{
		ImGui::TableSetupColumn("Event"); ImGui::TableSetupColumn("Action"); ImGui::TableSetupColumn("Count / X"); ImGui::TableSetupColumn("Instances / Y"); ImGui::TableSetupColumn("GPU ms"); ImGui::TableSetupColumn("Region"); ImGui::TableHeadersRow();
		std::vector<size_t> order; for (size_t i = 0; i < s.frame.size(); ++i) order.push_back(i);
		if (s.expensive_first) std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return gpu_ms(s.frame[a]) > gpu_ms(s.frame[b]); });
		for (size_t i : order)
		{
			const auto &record = s.frame[i]; std::string searchable = record.kind + " " + record.marker;
			for (auto shader : record.shaders) searchable += " " + hex_id(shader);
			for (auto &res : record.resources) searchable += " " + res.id;
			if (!matches_filter(s, searchable)) continue;
			ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow(); ImGui::TableNextColumn();
			const auto label = std::to_string(i + 1) + (record.skipped ? " [skip]" : record.modified ? " [mod]" : "");
			if (ImGui::Selectable(label.c_str(), s.selected_draw == static_cast<int>(i), ImGuiSelectableFlags_SpanAllColumns)) s.selected_draw = static_cast<int>(i);
			ImGui::TableNextColumn(); ImGui::TextUnformatted(record.kind.c_str()); ImGui::TableNextColumn(); ImGui::Text("%u", record.count);
			ImGui::TableNextColumn(); ImGui::Text("%u", record.instances); ImGui::TableNextColumn(); show_gpu_time(record);
			ImGui::TableNextColumn(); ImGui::TextUnformatted(record.marker.c_str()); ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::EndChild(); ImGui::SameLine();
	ImGui::BeginChild("draw_details", ImVec2(0, 450), ImGuiChildFlags_Borders);
	if (s.selected_draw >= 0 && static_cast<size_t>(s.selected_draw) < s.frame.size())
	{
		auto &record = s.frame[s.selected_draw];
		ImGui::Text("Event %d: %s", s.selected_draw + 1, record.kind.c_str());
		ImGui::Text("Count %u | Instances %u | First %u | Vertex offset %d", record.count, record.instances, record.first, record.vertex_offset);
		ImGui::Text("Command list %s", hex_id(record.command).c_str());
		ImGui::TextUnformatted("GPU ms:"); ImGui::SameLine(); show_gpu_time(record);
		if (!record.marker.empty()) ImGui::TextWrapped("%s", record.marker.c_str());
		if (ImGui::CollapsingHeader("Shaders", ImGuiTreeNodeFlags_DefaultOpen))
			for (auto hash : record.shaders) if (auto shader = s.shaders.find(hash); shader != s.shaders.end())
			{
				ImGui::PushID(hex_id(hash).c_str()); show_shader(s, shader->second); ImGui::Separator(); ImGui::PopID();
			}
		if (ImGui::CollapsingHeader("Bound resources", ImGuiTreeNodeFlags_DefaultOpen))
			for (auto &res : record.resources)
			{
				const auto label = res.role + ": " + res.id;
				if (ImGui::Selectable(label.c_str(), s.selected_resource == res.serial)) s.selected_resource = res.serial;
			}
		if (auto info = resource_by_serial(s, s.selected_resource)) show_resource(runtime, s, *info);
		if (ImGui::CollapsingHeader("Rasterizer / output state"))
		{
			for (size_t i = 0; i < record.viewports.size(); ++i) { const auto &v = record.viewports[i]; ImGui::Text("Viewport %zu: %.0f,%.0f %.0fx%.0f depth %.3f..%.3f", i, v.x, v.y, v.width, v.height, v.min_depth, v.max_depth); }
			for (size_t i = 0; i < record.scissors.size(); ++i) { const auto &r = record.scissors[i]; ImGui::Text("Scissor %zu: %d,%d to %d,%d", i, r.left, r.top, r.right, r.bottom); }
			for (auto &value : record.dynamic_states) ImGui::Text("State %u: %u (0x%08X)", static_cast<uint32_t>(value.first), value.second, value.second);
		}
	}
	else ImGui::TextWrapped("Capture a frame, then select an event to inspect its shaders, bound resources and output state.");
	ImGui::EndChild();
}
static void draw_panel(effect_runtime *runtime)
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	auto state = find(runtime->get_device()); if (!state) return; auto &s = *state;
	ImGui::Text("Mod Kit | %s", api_name(runtime->get_device()->get_api()));
	ImGui::BeginDisabled(s.comparison.active); ImGui::Checkbox("Enable mods", &s.enabled); ImGui::EndDisabled();
	ImGui::SameLine(); ImGui::Checkbox("Watch replacement files", &s.watch);
	ImGui::SameLine(); if (ImGui::Button("Reload mods")) s.reload_requested = true;
	ImGui::InputTextWithHint("##modkit_filter", "Filter actions, markers or resource/shader IDs", s.filter, sizeof(s.filter));
	if (ImGui::BeginTabBar("modkit_tabs"))
	{
		if (ImGui::BeginTabItem("Frame / pipeline")) { show_draws(runtime, s); ImGui::EndTabItem(); }
		if (ImGui::BeginTabItem("Profiler")) { show_profiler(runtime, s); ImGui::EndTabItem(); }
		if (ImGui::BeginTabItem("Shaders"))
		{
			ImGui::BeginChild("shader_list", ImVec2(220, 400), ImGuiChildFlags_Borders); for (auto &entry : s.shaders)
			{
				const auto label = std::string(stage_name(entry.second.stage)) + " " + hex_id(entry.first);
				if (matches_filter(s, label) && ImGui::Selectable(label.c_str(), s.selected_shader == entry.first)) s.selected_shader = entry.first;
			}
			ImGui::EndChild(); ImGui::SameLine(); ImGui::BeginChild("shader_detail", ImVec2(0, 400), ImGuiChildFlags_Borders);
			if (auto it = s.shaders.find(s.selected_shader); it != s.shaders.end()) show_shader(s, it->second);
			ImGui::EndChild(); ImGui::EndTabItem();
		}
		for (const bool textures : {true, false}) if (ImGui::BeginTabItem(textures ? "Textures" : "Buffers"))
		{
			ImGui::BeginChild("resource_list", ImVec2(240, 420), ImGuiChildFlags_Borders);
			std::vector<resource_info *> entries;
			for (auto &entry : s.resources) if ((entry.second.desc.type == resource_type::buffer) != textures) entries.push_back(&entry.second);
			std::sort(entries.begin(), entries.end(), [](auto a, auto b) { return a->id < b->id; });
			for (auto info : entries) if (matches_filter(s, info->id))
			{
				ImGui::PushID(static_cast<int>(info->serial));
				if (ImGui::Selectable(info->id.c_str(), s.selected_resource == info->serial)) s.selected_resource = info->serial;
				ImGui::PopID();
			}
			ImGui::EndChild(); ImGui::SameLine(); ImGui::BeginChild("resource_detail", ImVec2(0, 420), ImGuiChildFlags_Borders);
			if (auto info = resource_by_serial(s, s.selected_resource)) show_resource(runtime, s, *info);
			ImGui::EndChild(); ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Editor / rules"))
		{
			if (ImGui::Button("Edit draw rules")) load_editor(s, s.root / "rules.ini", "; Draw rules\n");
			ImGui::SameLine(); if (ImGui::Button("Save"))
			{
				if (!s.editor_path.empty() && write_binary(s.editor_path, s.editor.data(), std::strlen(s.editor.data()))) { s.reload_requested = true; report(s, "Saved " + s.editor_path.u8string()); }
			}
			ImGui::TextWrapped("%s", s.editor_path.u8string().c_str());
			ImGui::InputTextMultiline("##editor", s.editor.data(), s.editor.size(), ImVec2(-1, 350), ImGuiInputTextFlags_AllowTabInput);
			if (s.editor_path.extension() == ".hlsl")
			{
				uint64_t hash = 0; number(s.editor_path.stem().u8string(), hash, true);
				if (auto it = s.shaders.find(hash); it != s.shaders.end()) if (ImGui::Button("Compile HLSL and apply")) compile_editor(s, it->second);
			}
			ImGui::TextWrapped("Rules match shader ID, action kind, count and instances. Skip or modify draw parameters. First edits apply when the game records new draw commands.");
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Session"))
		{
			ImGui::Checkbox("Capture CPU uploads (adds copy and hashing overhead)", &s.capture_uploads);
			ImGui::TextWrapped("Off by default. Enable to capture resource contents and content IDs from new CPU uploads. Existing resources use session IDs until contents are captured.");
			ImGui::TextWrapped("Mod directory: %s", s.root.u8string().c_str());
			ImGui::Text("%zu shaders | %zu resources | %zu pipelines", s.shaders.size(), s.resources.size(), s.pipelines.size());
			ImGui::Text("Intercepted: %llu | Modified: %llu | Suppressed: %llu", static_cast<unsigned long long>(s.total_draws), static_cast<unsigned long long>(s.modified_draws), static_cast<unsigned long long>(s.skipped_draws));
			ImGui::Text("CPU snapshots: %.1f / 256 MiB", s.capture_bytes / 1048576.0);
			ImGui::Text("Recorded command metadata: %.1f / 64 MiB", s.action_bytes / 1048576.0);
			ImGui::Text("Retained GPU resources: %.1f / 512 MiB", s.replacement_bytes / 1048576.0);
			ImGui::Text("Retained pipeline versions: %zu / %zu | Descriptor snapshots: %zu / %zu", s.owned_pipelines.size(), max_pipelines, s.shadow_tables.size(), max_tables);
			ImGui::TextWrapped("Detailed draw metadata is collected only during capture, GPU timing or a suppression comparison. Cached commands may keep existing mods until the game records them again. Ray tracing replacement, bindless descriptor tables and hardware performance counters are outside the current implementation.");
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	ImGui::Separator(); ImGui::TextWrapped("%s", s.status.c_str());
}
}

#define MODKIT_EVENTS(X) \
	X(init_device, on_init_device) X(destroy_device, on_destroy_device) \
	X(init_command_queue, on_init_queue) X(destroy_command_queue, on_destroy_queue) \
	X(init_command_list, on_init_command) X(destroy_command_list, on_destroy_command) X(reset_command_list, on_reset_command) X(execute_secondary_command_list, on_secondary) X(execute_command_list, on_execute) \
	X(init_pipeline, on_init_pipeline) X(destroy_pipeline, on_destroy_pipeline) X(bind_pipeline, on_bind_pipeline) \
	X(init_pipeline_layout, on_init_layout) X(destroy_pipeline_layout, on_destroy_layout) \
	X(init_resource, on_init_resource) X(destroy_resource, on_destroy_resource) X(init_resource_view, on_init_view) X(destroy_resource_view, on_destroy_view) \
	X(update_texture_region, on_update_texture) X(update_buffer_region, on_update_buffer) X(update_texture_region_command, on_update_texture_cmd) X(update_buffer_region_command, on_update_buffer_cmd) \
	X(map_buffer_region, on_map_buffer) X(unmap_buffer_region, on_unmap_buffer) X(map_texture_region, on_map_texture) X(unmap_texture_region, on_unmap_texture) X(copy_buffer_to_texture, on_buffer_to_texture) \
	X(update_descriptor_tables, on_update_descriptors) X(copy_descriptor_tables, on_copy_descriptors) X(push_descriptors, on_push) X(bind_descriptor_tables, on_bind_tables) \
	X(bind_vertex_buffers, on_vertices) X(bind_index_buffer, on_index) X(bind_render_targets_and_depth_stencil, on_targets) X(begin_render_pass, on_render_pass) \
	X(bind_viewports, on_viewports) X(bind_scissor_rects, on_scissors) X(bind_pipeline_states, on_dynamic_states) \
	X(draw, on_draw) X(draw_indexed, on_draw_indexed) X(dispatch, on_dispatch) X(dispatch_mesh, on_mesh) X(draw_or_dispatch_indirect, on_indirect) X(present, on_present)

void register_addon_modkit()
{
	using namespace reshade::modkit;
#define REGISTER(event, callback) reshade::register_event<reshade::addon_event::event>(callback);
	MODKIT_EVENTS(REGISTER)
#undef REGISTER
	reshade::register_overlay("Mod Kit", draw_panel);
}
void unregister_addon_modkit()
{
	using namespace reshade::modkit;
	reshade::unregister_overlay("Mod Kit", draw_panel);
#define UNREGISTER(event, callback) reshade::unregister_event<reshade::addon_event::event>(callback);
	MODKIT_EVENTS(UNREGISTER)
#undef UNREGISTER
}
#undef MODKIT_EVENTS

extern "C" __declspec(dllexport) const char *NAME = "Mod Kit";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Live graphics inspection, resource replacement and draw modding in ReShade.";
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		if (!reshade::register_addon(module)) return FALSE;
		register_addon_modkit();
	}
	else if (reason == DLL_PROCESS_DETACH) { unregister_addon_modkit(); reshade::modkit::remove_timing_hooks(); reshade::unregister_addon(module); }
	return TRUE;
}
