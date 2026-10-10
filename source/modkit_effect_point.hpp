/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once
#include "modkit_profile.hpp"
#include <algorithm>
#include <sstream>
#include <windows.h>

namespace reshade::modkit
{
struct effect_point
{
	bool enabled = false, match_count = false;
	std::string kind;
	uint32_t count = 0;
	std::vector<uint64_t> shaders;
	// Old execution-point files did not include a target, so preserve their
	// original back-buffer-only behavior when loading them.
	bool back_buffer = true;
	uint32_t target_width = 0, target_height = 0, target_format = 0;
};
inline bool load_effect_point(const std::filesystem::path &path, effect_point &point)
{
	std::error_code ec;
	if (!std::filesystem::exists(path, ec)) return !ec;
	if (std::filesystem::file_size(path, ec) > 4096 || ec) return false;
	std::ifstream file(path); if (!file) return false;
	effect_point loaded; std::string line;
	while (std::getline(file, line))
	{
		line = trim(line); if (line.empty() || line[0] == '#') continue;
		const auto equals = line.find('='); if (equals == std::string::npos) return false;
		const auto name = trim(line.substr(0, equals)), value = trim(line.substr(equals + 1));
		uint64_t parsed = 0;
		if (name == "kind") { if (value != "draw" && value != "indexed") return false; loaded.kind = value; }
		else if (name == "shaders")
		{
			std::istringstream ids(value); std::string id;
			while (ids >> id) { if (id.size() != 16 || !number(id, parsed, true) || loaded.shaders.size() >= 16) return false; loaded.shaders.push_back(parsed); }
		}
		else if (name == "enabled" || name == "match_count")
		{
			if (value != "0" && value != "1") return false;
			(name == "enabled" ? loaded.enabled : loaded.match_count) = value == "1";
		}
		else if (name == "count") { if (!number(value, parsed) || parsed > UINT32_MAX) return false; loaded.count = static_cast<uint32_t>(parsed); }
		else if (name == "target") { if (value == "back_buffer") loaded.back_buffer = true; else if (value == "texture") loaded.back_buffer = false; else return false; }
		else if (name == "target_width" || name == "target_height" || name == "target_format")
		{
			if (!number(value, parsed) || parsed > UINT32_MAX) return false;
			if (name == "target_width") loaded.target_width = static_cast<uint32_t>(parsed);
			else if (name == "target_height") loaded.target_height = static_cast<uint32_t>(parsed);
			else loaded.target_format = static_cast<uint32_t>(parsed);
		}
		else return false;
	}
	if (file.bad() || loaded.kind.empty() || loaded.shaders.empty()) return false;
	if (!loaded.back_buffer && (!loaded.target_width || !loaded.target_height || !loaded.target_format)) return false;
	std::sort(loaded.shaders.begin(), loaded.shaders.end());
	loaded.shaders.erase(std::unique(loaded.shaders.begin(), loaded.shaders.end()), loaded.shaders.end());
	point = std::move(loaded); return true;
}
inline bool save_effect_point(const std::filesystem::path &path, const effect_point &point)
{
	if (point.shaders.empty()) { std::error_code ec; std::filesystem::remove(path, ec); return !ec; }
	std::string text = "# Mod Kit early ReShade effects: first matching draw and output target.\n";
	text += "enabled=" + std::to_string(point.enabled) + "\nkind=" + point.kind + "\nmatch_count=" + std::to_string(point.match_count) + "\ncount=" + std::to_string(point.count) + "\nshaders=";
	for (auto shader : point.shaders) text += hex_id(shader) + ' ';
	text += "\ntarget=" + std::string(point.back_buffer ? "back_buffer" : "texture") + "\ntarget_width=" + std::to_string(point.target_width) + "\ntarget_height=" + std::to_string(point.target_height) + "\ntarget_format=" + std::to_string(point.target_format);
	text += '\n';
	auto temporary = path; temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId());
	if (!write_binary(temporary, text.data(), text.size())) return false;
	if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
	DeleteFileW(temporary.c_str()); return false;
}
}
