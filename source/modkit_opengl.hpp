/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace reshade::modkit
{
// Resolve through the application's current context so the companion add-on can
// preserve uniforms without depending on ReShade's private OpenGL implementation.
template <typename T> inline T gl_function(const char *name)
{
	static HMODULE module = GetModuleHandleW(L"opengl32.dll");
	if (!module) return nullptr;
	using get_context = HGLRC(WINAPI *)();
	using get_proc = PROC(WINAPI *)(LPCSTR);
	static auto context = reinterpret_cast<get_context>(GetProcAddress(module, "wglGetCurrentContext"));
	static auto resolve = reinterpret_cast<get_proc>(GetProcAddress(module, "wglGetProcAddress"));
	thread_local HGLRC previous = nullptr;
	thread_local std::map<std::string, PROC> cache;
	auto current = context ? context() : nullptr;
	if (!current) return nullptr;
	if (current != previous) { cache.clear(); previous = current; }
	auto found = cache.find(name);
	if (found == cache.end())
	{
		auto address = resolve ? resolve(name) : nullptr;
		if (reinterpret_cast<uintptr_t>(address) <= 3 || reinterpret_cast<uintptr_t>(address) == UINTPTR_MAX) address = nullptr;
		if (!address) address = reinterpret_cast<PROC>(GetProcAddress(module, name));
		found = cache.emplace(name, address).first;
	}
	return reinterpret_cast<T>(found->second);
}

struct gl_program_state
{
	int program = 0;
	bool compatibility = false, alpha = false;
	gl_program_state()
	{
		auto get = gl_function<void(APIENTRY *)(unsigned, int *)>("glGetIntegerv");
		auto version = gl_function<const unsigned char *(APIENTRY *)(unsigned)>("glGetString");
		auto enabled = gl_function<unsigned char(APIENTRY *)(unsigned)>("glIsEnabled");
		if (!get || !version || !enabled) return;
		get(0x8B8D, &program); // GL_CURRENT_PROGRAM
		const auto text = reinterpret_cast<const char *>(version(0x1F02));
		if (!text) return;
		// Querying alpha-test state in a core context would itself cause a GL error.
		compatibility = text[0] < '3' || (text[0] == '3' && text[1] == '.' && text[2] < '2');
		if (!compatibility) { int mask = 0; get(0x9126, &mask); compatibility = (mask & 2) != 0; }
		if (compatibility) alpha = enabled(0x0BC0) != 0;
	}
	void restore_alpha() const
	{
		if (compatibility) if (auto set = gl_function<void(APIENTRY *)(unsigned)>(alpha ? "glEnable" : "glDisable")) set(0x0BC0);
	}
};

struct gl_uniform_shape { char kind = 0; int columns = 1, rows = 1; };
inline bool gl_supports(int major, int minor, const char *extension_name)
{
	auto text = gl_function<const unsigned char *(APIENTRY *)(unsigned)>("glGetString");
	auto get = gl_function<void(APIENTRY *)(unsigned, int *)>("glGetIntegerv");
	if (!text || !get) return false;
	const char *version = reinterpret_cast<const char *>(text(0x1F02));
	if (!version || std::strlen(version) < 3) return false;
	if (version[0] - '0' > major || (version[0] - '0' == major && version[2] - '0' >= minor)) return true;
	if (version[0] >= '3')
	{
		if (auto extension = gl_function<const unsigned char *(APIENTRY *)(unsigned, unsigned)>("glGetStringi"))
		{
			int count = 0; get(0x821D, &count);
			if (count < 0 || count > 65536) return false;
			for (int i = 0; i < count; ++i) if (auto name = extension(0x1F03, i))
				if (std::strcmp(reinterpret_cast<const char *>(name), extension_name) == 0) return true;
		}
	}
	else if (auto list = reinterpret_cast<const char *>(text(0x1F03)))
	{
		const size_t length = std::strlen(extension_name);
		for (auto match = std::strstr(list, extension_name); match; match = std::strstr(match + length, extension_name))
			if ((match == list || match[-1] == ' ') && (match[length] == 0 || match[length] == ' ')) return true;
	}
	return false;
}
inline gl_uniform_shape uniform_shape(unsigned type)
{
	switch (type)
	{
	case 0x1406: return {'f', 1, 1};
	case 0x8B50: case 0x8B51: case 0x8B52: return {'f', 1, int(type - 0x8B50 + 2)};
	case 0x1404: case 0x8B56: return {'i', 1, 1};
	case 0x8B53: case 0x8B54: case 0x8B55: return {'i', 1, int(type - 0x8B53 + 2)};
	case 0x8B57: case 0x8B58: case 0x8B59: return {'i', 1, int(type - 0x8B57 + 2)};
	case 0x1405: return {'u', 1, 1};
	case 0x8DC6: case 0x8DC7: case 0x8DC8: return {'u', 1, int(type - 0x8DC6 + 2)};
	case 0x140A: return {'d', 1, 1};
	case 0x8FFC: case 0x8FFD: case 0x8FFE: return {'d', 1, int(type - 0x8FFC + 2)};
	case 0x8B5A: case 0x8B5B: case 0x8B5C: return {'f', int(type - 0x8B5A + 2), int(type - 0x8B5A + 2)};
	case 0x8B65: return {'f', 2, 3}; case 0x8B66: return {'f', 2, 4};
	case 0x8B67: return {'f', 3, 2}; case 0x8B68: return {'f', 3, 4};
	case 0x8B69: return {'f', 4, 2}; case 0x8B6A: return {'f', 4, 3};
	case 0x8F46: case 0x8F47: case 0x8F48: return {'d', int(type - 0x8F46 + 2), int(type - 0x8F46 + 2)};
	case 0x8F49: return {'d', 2, 3}; case 0x8F4A: return {'d', 2, 4};
	case 0x8F4B: return {'d', 3, 2}; case 0x8F4C: return {'d', 3, 4};
	case 0x8F4D: return {'d', 4, 2}; case 0x8F4E: return {'d', 4, 3};
	// Sampler and image uniforms contain integer texture/image unit indices.
	case 0x8B5D: case 0x8B5E: case 0x8B5F: case 0x8B60: case 0x8B61: case 0x8B62: case 0x8B63: case 0x8B64:
	case 0x8DC0: case 0x8DC1: case 0x8DC2: case 0x8DC3: case 0x8DC4: case 0x8DC5:
	case 0x8DC9: case 0x8DCA: case 0x8DCB: case 0x8DCC: case 0x8DCD: case 0x8DCE: case 0x8DCF: case 0x8DD0:
	case 0x8DD1: case 0x8DD2: case 0x8DD3: case 0x8DD4: case 0x8DD5: case 0x8DD6: case 0x8DD7: case 0x8DD8:
	case 0x900C: case 0x900D: case 0x900E: case 0x900F: case 0x9108: case 0x9109: case 0x910A: case 0x910B: case 0x910C: case 0x910D:
		return {'i', 1, 1};
	default:
		if (type >= 0x904C && type <= 0x906C) return {'i', 1, 1}; // float/int/uint image types
		return {};
	}
}

template <typename T> inline bool copy_gl_uniform(unsigned source, int from, int to, gl_uniform_shape shape)
{
	const char suffix = shape.kind == 'u' ? 'i' : shape.kind;
	std::string getter = "glGetUniform"; getter += shape.kind == 'u' ? "uiv" : std::string(1, suffix) + "v";
	std::string setter = "glUniform";
	if (shape.columns > 1)
	{
		setter += "Matrix" + std::to_string(shape.columns);
		if (shape.rows != shape.columns) setter += "x" + std::to_string(shape.rows);
	}
	else setter += std::to_string(shape.rows);
	setter += shape.kind == 'u' ? "uiv" : std::string(1, suffix) + "v";
	auto get = gl_function<void(APIENTRY *)(unsigned, int, T *)>(getter.c_str());
	std::array<T, 16> value{};
	if (!get) return false;
	if (shape.columns > 1)
	{
		auto set = gl_function<void(APIENTRY *)(int, int, unsigned char, const T *)>(setter.c_str());
		if (!set) return false;
		get(source, from, value.data()); set(to, 1, false, value.data());
	}
	else
	{
		auto set = gl_function<void(APIENTRY *)(int, int, const T *)>(setter.c_str());
		if (!set) return false;
		get(source, from, value.data()); set(to, 1, value.data());
	}
	return true;
}

inline bool copy_gl_program(unsigned source)
{
	auto get = gl_function<void(APIENTRY *)(unsigned, int *)>("glGetIntegerv");
	auto info = gl_function<void(APIENTRY *)(unsigned, unsigned, int *)>("glGetProgramiv");
	auto active = gl_function<void(APIENTRY *)(unsigned, unsigned, int, int *, int *, unsigned *, char *)>("glGetActiveUniform");
	auto location = gl_function<int(APIENTRY *)(unsigned, const char *)>("glGetUniformLocation");
	if (!source || !get || !info || !active || !location) return false;
	int target = 0; get(0x8B8D, &target);
	if (!target) return false;
	// A relink can choose different locations when the game used pre-link bindings.
	// Keep the original program whenever that would reinterpret its vertex array.
	auto attribute = gl_function<void(APIENTRY *)(unsigned, unsigned, int, int *, int *, unsigned *, char *)>("glGetActiveAttrib");
	auto attribute_location = gl_function<int(APIENTRY *)(unsigned, const char *)>("glGetAttribLocation");
	if (!attribute || !attribute_location) return false;
	int attributes = 0, name_length = 0; info(source, 0x8B89, &attributes); info(source, 0x8B8A, &name_length);
	if (attributes < 0 || attributes > 65536 || name_length > 65536) return false;
	std::vector<char> attribute_name(std::max(1, name_length));
	for (int i = 0; i < attributes; ++i)
	{
		int size = 0; unsigned type = 0; attribute(source, i, name_length, nullptr, &size, &type, attribute_name.data());
		const int a = attribute_location(source, attribute_name.data()), b = attribute_location(target, attribute_name.data());
		if (b >= 0 && a != b) return false;
	}
	if (gl_supports(3, 0, "GL_EXT_transform_feedback"))
	{
		int varyings = 0; info(source, 0x8C83, &varyings);
		if (varyings) return false; // The generic snapshot does not retain feedback declarations.
	}
	if (gl_supports(4, 3, "GL_ARB_program_interface_query"))
	{
		auto interface_info = gl_function<void(APIENTRY *)(unsigned, unsigned, unsigned, int *)>("glGetProgramInterfaceiv");
		auto name = gl_function<void(APIENTRY *)(unsigned, unsigned, unsigned, int, int *, char *)>("glGetProgramResourceName");
		auto index = gl_function<unsigned(APIENTRY *)(unsigned, unsigned, const char *)>("glGetProgramResourceIndex");
		auto properties = gl_function<void(APIENTRY *)(unsigned, unsigned, unsigned, int, const unsigned *, int, int *, int *)>("glGetProgramResourceiv");
		if (!interface_info || !name || !index || !properties) return false;
		int outputs = 0; interface_info(source, 0x92E4, 0x92F5, &outputs);
		for (int i = 0; i < outputs; ++i)
		{
			unsigned props[] = {0x92F9, 0x930E, 0x930F}; int values[3] = {};
			properties(source, 0x92E4, i, 3, props, 3, nullptr, values);
			if (values[0] <= 0 || values[0] > 65536) return false;
			std::vector<char> text(values[0]); name(source, 0x92E4, i, values[0], nullptr, text.data());
			auto dest = index(target, 0x92E4, text.data()); if (dest == ~0u) continue;
			int binding[2] = {}; properties(target, 0x92E4, dest, 2, props + 1, 2, nullptr, binding);
			if (binding[0] != values[1] || binding[1] != values[2]) return false;
		}
	}
	struct uniform { int count; unsigned type; };
	auto uniforms = [&](unsigned program)
	{
		std::map<std::string, uniform> result;
		int count = 0, length = 0; info(program, 0x8B86, &count); info(program, 0x8B87, &length);
		if (count < 0 || count > 65536 || length <= 0 || length > 65536) return result;
		std::vector<char> name(length);
		for (int i = 0; i < count; ++i) { int size = 0; unsigned type = 0; active(program, i, length, nullptr, &size, &type, name.data()); result.emplace(name.data(), uniform{size, type}); }
		return result;
	};
	auto from = uniforms(source), to = uniforms(target);
	// Reject changed interfaces before issuing any draw with the replacement.
	for (const auto &u : from) if (auto dest = to.find(u.first); dest != to.end() && location(source, u.first.c_str()) >= 0 && location(target, u.first.c_str()) >= 0)
		if (u.second.type != dest->second.type || !uniform_shape(u.second.type).kind) return false;
	for (const auto &u : from)
	{
		auto dest = to.find(u.first); if (dest == to.end()) continue;
		for (int i = 0; i < u.second.count && i < dest->second.count; ++i)
		{
			auto name = u.first;
			if (auto bracket = name.rfind("[0]"); bracket != std::string::npos) name.replace(bracket, 3, "[" + std::to_string(i) + "]");
			auto a = location(source, name.c_str()), b = location(target, name.c_str());
			if (a < 0 || b < 0) continue; // Uniform blocks and optimized-out values.
			auto shape = uniform_shape(u.second.type); bool success = false;
			switch (shape.kind)
			{
			case 'f': success = copy_gl_uniform<float>(source, a, b, shape); break;
			case 'i': success = copy_gl_uniform<int>(source, a, b, shape); break;
			case 'u': success = copy_gl_uniform<unsigned>(source, a, b, shape); break;
			case 'd': success = copy_gl_uniform<double>(source, a, b, shape); break;
			}
			if (!success) return false;
		}
	}
	// Preserve named uniform-buffer and shader-storage-buffer binding points.
	int blocks = 0;
	if (gl_supports(3, 1, "GL_ARB_uniform_buffer_object")) info(source, 0x8A36, &blocks);
	if (blocks)
	{
		auto name = gl_function<void(APIENTRY *)(unsigned, unsigned, int, int *, char *)>("glGetActiveUniformBlockName");
		auto index = gl_function<unsigned(APIENTRY *)(unsigned, const char *)>("glGetUniformBlockIndex");
		auto binding = gl_function<void(APIENTRY *)(unsigned, unsigned, unsigned, int *)>("glGetActiveUniformBlockiv");
		auto set = gl_function<void(APIENTRY *)(unsigned, unsigned, unsigned)>("glUniformBlockBinding");
		if (!name || !index || !binding || !set) return false;
		for (int i = 0; i < blocks; ++i)
		{
			int length = 0, point = 0; binding(source, i, 0x8A41, &length); binding(source, i, 0x8A3F, &point);
			if (length <= 0 || length > 65536) return false;
			std::vector<char> text(length); name(source, i, length, nullptr, text.data());
			auto dest = index(target, text.data()); if (dest != ~0u) set(target, dest, point);
		}
	}
	auto interface_info = gl_function<void(APIENTRY *)(unsigned, unsigned, unsigned, int *)>("glGetProgramInterfaceiv");
	const bool storage_blocks = gl_supports(4, 3, "GL_ARB_shader_storage_buffer_object");
	if (storage_blocks && !interface_info) return false;
	if (storage_blocks)
	{
		auto name = gl_function<void(APIENTRY *)(unsigned, unsigned, unsigned, int, int *, char *)>("glGetProgramResourceName");
		auto index = gl_function<unsigned(APIENTRY *)(unsigned, unsigned, const char *)>("glGetProgramResourceIndex");
		auto properties = gl_function<void(APIENTRY *)(unsigned, unsigned, unsigned, int, const unsigned *, int, int *, int *)>("glGetProgramResourceiv");
		auto set = gl_function<void(APIENTRY *)(unsigned, unsigned, unsigned)>("glShaderStorageBlockBinding");
		if (!name || !index || !properties || !set) return false;
		blocks = 0; interface_info(source, 0x92E6, 0x92F5, &blocks);
		for (int i = 0; i < blocks; ++i)
		{
			unsigned props[] = {0x92F9, 0x9302}; int values[2] = {};
			properties(source, 0x92E6, i, 2, props, 2, nullptr, values);
			if (values[0] <= 0 || values[0] > 65536) return false;
			std::vector<char> text(values[0]); name(source, 0x92E6, i, values[0], nullptr, text.data());
			auto dest = index(target, 0x92E6, text.data()); if (dest != ~0u) set(target, dest, values[1]);
		}
	}
	return true;
}
}
