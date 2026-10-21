/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace reshade::modkit
{

inline uint64_t hash_bytes(const void *data, size_t size, uint64_t hash = 14695981039346656037ull)
{
	const auto bytes = static_cast<const uint8_t *>(data);
	for (size_t i = 0; i < size; ++i)
		hash = (hash ^ bytes[i]) * 1099511628211ull;
	return hash;
}
inline std::string hex_id(uint64_t value)
{
	char result[17];
	constexpr char digits[] = "0123456789ABCDEF";
	for (int i = 15; i >= 0; --i, value >>= 4)
		result[i] = digits[value & 15];
	result[16] = 0;
	return result;
}
enum class profiler_frame_hint { none, never_captured, awaiting_readback, captured_empty };
// Maps the displayed frame's state to the hint the profiler tabs should show.
inline profiler_frame_hint profiler_frame_hint_state(bool frame_empty, uint64_t displayed_frame_index, size_t pending_frames)
{
	if (!frame_empty) return profiler_frame_hint::none;
	if (displayed_frame_index == 0 && pending_frames > 0) return profiler_frame_hint::awaiting_readback;
	if (displayed_frame_index > 0) return profiler_frame_hint::captured_empty;
	return profiler_frame_hint::never_captured;
}
inline std::string trim(std::string value)
{
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return {};
	return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
inline bool number(std::string text, uint64_t &value, bool hexadecimal = false)
{
	text = trim(text);
	if (text.substr(0, 2) == "0x" || text.substr(0, 2) == "0X")
	{
		text.erase(0, 2);
		hexadecimal = true;
	}
	const auto result = std::from_chars(text.data(), text.data() + text.size(), value, hexadecimal ? 16 : 10);
	return !text.empty() && result.ec == std::errc() && result.ptr == text.data() + text.size();
}

struct draw_rule
{
	std::string name, kind = "any";
	std::optional<uint64_t> shader;
	std::optional<uint32_t> count, instances, replace_count, replace_instances;
	bool skip = false;
};

// Parse into a new profile. The caller publishes it only after the entire file succeeds.
inline bool read_rules(const std::filesystem::path &path, std::vector<draw_rule> &rules, std::string &error)
{
	std::ifstream file(path);
	if (!file) { error = "Cannot open rules.ini"; return false; }
	std::vector<draw_rule> parsed;
	std::string line;
	size_t line_number = 0;
	while (std::getline(file, line))
	{
		++line_number;
		line = trim(line);
		if (line.empty() || line[0] == '#' || line[0] == ';') continue;
		if (line[0] == '[' && line.back() == ']')
		{
			if (parsed.size() >= 256) { error = "At most 256 draw rules are allowed"; return false; }
			parsed.push_back({});
			parsed.back().name = line.substr(1, line.size() - 2);
			continue;
		}
		const auto equals = line.find('=');
		bool valid = !parsed.empty() && equals != std::string::npos;
		if (valid)
		{
			auto &rule = parsed.back();
			const auto key = trim(line.substr(0, equals)), value = trim(line.substr(equals + 1));
			uint64_t integer = 0;
			if (key == "kind")
			{
				valid = value == "any" || value == "draw" || value == "indexed" || value == "dispatch" || value == "mesh" || value == "indirect";
				rule.kind = value;
			}
			else if (key == "action") { valid = value == "skip" || value == "modify"; rule.skip = value == "skip"; }
			else if (key == "shader") { valid = number(value, integer, true); rule.shader = integer; }
			else
			{
				valid = number(value, integer) && integer <= UINT32_MAX;
				if (key == "count") rule.count = static_cast<uint32_t>(integer);
				else if (key == "instances") rule.instances = static_cast<uint32_t>(integer);
				else if (key == "replace_count") rule.replace_count = static_cast<uint32_t>(integer);
				else if (key == "replace_instances") rule.replace_instances = static_cast<uint32_t>(integer);
				else valid = false;
			}
		}
		if (!valid) { error = "Invalid rules.ini line " + std::to_string(line_number); return false; }
	}
	if (file.bad()) { error = "Failed to read rules.ini"; return false; }
	rules = std::move(parsed);
	error.clear();
	return true;
}

inline bool read_binary(const std::filesystem::path &path, std::vector<uint8_t> &out)
{
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) return false;
	const auto length = file.tellg();
	if (length <= 0 || length > 64 * 1024 * 1024) return false;
	out.resize(static_cast<size_t>(length));
	file.seekg(0);
	return static_cast<bool>(file.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size())));
}
inline bool write_binary(const std::filesystem::path &path, const void *data, size_t size)
{
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	return file && static_cast<bool>(file.write(static_cast<const char *>(data), static_cast<std::streamsize>(size)));
}

}
