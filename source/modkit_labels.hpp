/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once
#include "modkit_profile.hpp"
#include <map>
#include <windows.h>

namespace reshade::modkit
{
using shader_label_map = std::map<uint64_t, std::string>;
inline std::string clean_shader_label(std::string label)
{
	for (auto &c : label) if (c == '\t' || c == '\r' || c == '\n') c = ' ';
	return trim(std::move(label));
}
inline bool load_shader_labels(const std::filesystem::path &path, shader_label_map &labels, std::string &error)
{
	std::error_code ec;
	if (!std::filesystem::exists(path, ec)) return !ec;
	const auto bytes = std::filesystem::file_size(path, ec);
	if (ec || bytes > 3 * 1024 * 1024) { error = "Shader labels file cannot be read or exceeds 3 MiB."; return false; }
	std::ifstream input(path, std::ios::binary);
	if (!input) { error = "Could not open shader-labels.tsv."; return false; }
	shader_label_map loaded;
	std::string line; size_t row = 0;
	while (std::getline(input, line))
	{
		++row;
		if (row == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
		if (line.empty() || line[0] == '#') continue;
		const auto tab = line.find('\t'); uint64_t hash = 0;
		if (tab != 16 || line.size() > 16 + 1 + 255 + 1 || !number(line.substr(0, tab), hash, true) || line.find('\0') != std::string::npos || loaded.size() >= 8192)
		{ error = "Invalid shader-labels.tsv row " + std::to_string(row) + "; previous labels retained."; return false; }
		auto label = clean_shader_label(line.substr(tab + 1));
		if (!label.empty()) loaded[hash] = std::move(label);
	}
	if (input.bad()) { error = "Could not finish reading shader-labels.tsv."; return false; }
	labels = std::move(loaded); return true;
}
inline bool save_shader_labels(const std::filesystem::path &path, const shader_label_map &labels)
{
	std::string text = "# Mod Kit shader labels: bytecode hash followed by a tab and a UTF-8 label.\n";
	for (const auto &label : labels) text += hex_id(label.first) + '\t' + clean_shader_label(label.second) + '\n';
	auto temporary = path; temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId());
	if (!write_binary(temporary, text.data(), text.size())) return false;
	if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
	DeleteFileW(temporary.c_str()); return false;
}
}
