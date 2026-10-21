/* SPDX-License-Identifier: BSD-3-Clause */
#include "modkit_profile.hpp"
#ifdef _WIN32
#include "modkit_labels.hpp"
#include "modkit_effect_point.hpp"
#include "modkit_texture_barrier.hpp"
#else
#include <unistd.h>
#endif
#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace reshade::modkit;
static void expect(bool condition, const char *message)
{ if (!condition) throw std::runtime_error(message); }
struct temporary_directory
{
	std::filesystem::path path;
	temporary_directory()
	{
#ifdef _WIN32
		const auto pid = GetCurrentProcessId();
#else
		const auto pid = getpid();
#endif
		path = std::filesystem::temp_directory_path() / ("modkit-profile-checks-" + std::to_string(pid) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		expect(std::filesystem::create_directory(path), "Could not create unique check directory");
	}
	~temporary_directory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
static void put(const std::filesystem::path &path, const std::string &contents)
{ expect(write_binary(path, contents.data(), contents.size()), "Could not write check fixture"); }
int main()
{
	try
	{
		temporary_directory temporary;
		uint64_t value = 0;
		expect(number("FFFFFFFFFFFFFFFF", value, true) && value == UINT64_MAX, "Maximum shader ID");
		expect(number(" 0x1234 ", value) && value == 0x1234, "Prefixed hexadecimal ID");
		for (const auto *invalid : {"", "0x", "-1", "123junk", "18446744073709551616"}) expect(!number(invalid, value), "Invalid number accepted");
		expect(hex_id(1) == "0000000000000001", "Shader ID formatting");
		expect(hash_bytes("hello", 5) == 0xA430D84680AABD0Bull, "Stable FNV shader hash");
		expect(profiler_frame_hint_state(false, 0, 0) == profiler_frame_hint::none && profiler_frame_hint_state(false, 7, 2) == profiler_frame_hint::none, "Non-empty frame shows no hint");
		expect(profiler_frame_hint_state(true, 0, 0) == profiler_frame_hint::never_captured, "Empty frame before any capture is never captured");
		expect(profiler_frame_hint_state(true, 0, 1) == profiler_frame_hint::awaiting_readback && profiler_frame_hint_state(true, 0, 4) == profiler_frame_hint::awaiting_readback, "Queued frames before the first readback await it");
		expect(profiler_frame_hint_state(true, 3, 0) == profiler_frame_hint::captured_empty && profiler_frame_hint_state(true, 3, 2) == profiler_frame_hint::captured_empty, "Promoted empty frame reports captured empty");
		const auto rules_path = temporary.path / "rules.ini";
		put(rules_path, "[clouds]\nkind=indexed\nshader=0123456789ABCDEF\ncount=0\naction=skip\n[change]\nkind=draw\naction=modify\nreplace_instances=0\n");
		std::vector<draw_rule> rules; std::string error;
		expect(read_rules(rules_path, rules, error) && rules.size() == 2, "Rules round trip");
		expect(rules[0].shader == 0x0123456789ABCDEFull && rules[0].count == 0 && rules[0].skip, "Suppression rule values");
		expect(rules[1].replace_instances == 0, "Zero replacement value");
		for (const auto *invalid : {"count=3\n", "[bad]\nkind=unknown\n", "[bad]\ncount=4294967296\n", "[bad]\nshader=xyz\n"})
		{
			put(rules_path, invalid);
			expect(!read_rules(rules_path, rules, error) && rules.size() == 2 && rules[0].name == "clouds", "Invalid rules replaced the previous profile");
		}
		std::string oversized_rules; for (int i = 0; i < 257; ++i) oversized_rules += "[rule]\naction=skip\n";
		put(rules_path, oversized_rules); expect(!read_rules(rules_path, rules, error), "Rule count limit");
		const auto binary_path = temporary.path / "binary.bin";
		put(binary_path, std::string("A\0B", 3)); std::vector<uint8_t> bytes;
		expect(read_binary(binary_path, bytes) && bytes.size() == 3 && bytes[1] == 0, "Binary contents");
#ifdef _WIN32
		const auto labels_path = temporary.path / "shader-labels.tsv";
		shader_label_map labels {{1, "  Sky\tshader  "}, {UINT64_MAX, u8"Car métal"}}, loaded_labels;
		expect(save_shader_labels(labels_path, labels), "Save shader labels");
		expect(load_shader_labels(labels_path, loaded_labels, error) && loaded_labels.size() == 2 && loaded_labels.at(1) == "Sky shader" && loaded_labels.at(UINT64_MAX) == u8"Car métal", "Persistent UTF-8 labels");
		put(labels_path, "invalid-hash\tBad\n");
		expect(!load_shader_labels(labels_path, loaded_labels, error) && loaded_labels.size() == 2, "Malformed labels replaced previous labels");
		put(labels_path, std::string(3 * 1024 * 1024 + 1, 'x'));
		expect(!load_shader_labels(labels_path, loaded_labels, error), "Label size limit");
		put(labels_path, "\xEF\xBB\xBF# labels\r\n0000000000000002\tClouds\r\n");
		expect(load_shader_labels(labels_path, loaded_labels, error) && loaded_labels.at(2) == "Clouds", "BOM and CRLF labels");
		const auto point_path = temporary.path / "effect-point.ini";
		effect_point point {true, true, "indexed", 240, {UINT64_MAX, 1}}, loaded_point;
		point.back_buffer = false; point.target_width = 1920; point.target_height = 1080; point.target_format = static_cast<uint32_t>(reshade::api::format::r8g8b8a8_unorm);
		expect(save_effect_point(point_path, point) && load_effect_point(point_path, loaded_point), "Execution point round trip");
		expect(loaded_point.enabled && loaded_point.match_count && loaded_point.count == 240 && loaded_point.shaders == std::vector<uint64_t>({1, UINT64_MAX}) && !loaded_point.back_buffer && loaded_point.target_width == 1920 && loaded_point.target_height == 1080 && loaded_point.target_format == static_cast<uint32_t>(reshade::api::format::r8g8b8a8_unorm), "Execution point matching values");
		for (const auto *invalid : {"kind=dispatch\nshaders=0000000000000001\n", "enabled=2\nkind=draw\nshaders=0000000000000001\n", "kind=draw\ncount=4294967296\nshaders=0000000000000001\n", "kind=draw\nshaders=garbage\n"})
		{
			put(point_path, invalid);
			expect(!load_effect_point(point_path, loaded_point) && loaded_point.kind == "indexed", "Invalid execution point replaced previous settings");
		}
		put(point_path, "enabled=1\nkind=draw\nshaders=0000000000000001\n");
		expect(load_effect_point(point_path, loaded_point) && loaded_point.back_buffer, "Legacy execution point defaults to back buffer");
		put(point_path, "enabled=1\nkind=draw\ntarget=texture\ntarget_width=1920\ntarget_height=1080\nshaders=0000000000000001\n");
		expect(!load_effect_point(point_path, loaded_point) && loaded_point.back_buffer, "Off-screen point without target format rejected");
		expect(save_effect_point(point_path, {}) && !std::filesystem::exists(point_path), "Clear execution point");
		D3D12_RESOURCE_BARRIER barrier {}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = reinterpret_cast<ID3D12Resource *>(uintptr_t(0x1234));
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		expect(snapshot_barrier(barrier, 0x1234), "Full texture transition");
		barrier.Transition.Subresource = 0; expect(snapshot_barrier(barrier, 0x1234), "Base mip transition");
		barrier.Transition.Subresource = 1; expect(!snapshot_barrier(barrier, 0x1234), "Other mip cannot establish base mip state");
		barrier.Transition.Subresource = 0;
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY; expect(!snapshot_barrier(barrier, 0x1234), "Pending split transition cannot be read");
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_END_ONLY; expect(!snapshot_barrier(barrier, 0x1234), "Split barriers are unsupported");
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE; expect(!snapshot_barrier(barrier, 0x5678), "Wrong texture excluded");
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; expect(!snapshot_barrier(barrier, 0x1234), "UAV barrier is not a transition");
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING; expect(!snapshot_barrier(barrier, 0x1234), "Aliasing is not a transition");
		expect(preview_format(reshade::api::format::r8g8b8a8_typeless) && preview_format(reshade::api::format::bc7_unorm) && preview_format(reshade::api::format::r16g16b16a16_float), "Color preview formats");
		expect(!preview_format(reshade::api::format::r32_uint) && !preview_format(reshade::api::format::d24_unorm_s8_uint), "Integer and depth previews need a conversion shader");
#endif
		std::cout << "Profile checks passed\n";
		return 0;
	}
	catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
