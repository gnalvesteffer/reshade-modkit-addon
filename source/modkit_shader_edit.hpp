/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <d3d12shader.h>
#include "modkit_disassembly.hpp"
#include <vector>
#include <cstdint>
#include <sstream>
#include <type_traits>
#include <iterator>
#include <fstream>
#include <map>
#include <cctype>

namespace reshade::modkit
{
template <typename T> struct shader_com
{
	T *ptr = nullptr;
	shader_com() = default;
	shader_com(const shader_com &) = delete;
	shader_com &operator=(const shader_com &) = delete;
	~shader_com() { if (ptr) ptr->Release(); }
	T *operator->() const { return ptr; }
};
struct shader_parameter
{
	std::string semantic, type;
	uint32_t index = 0, system = 0;
	bool integer = false;
};
struct shader_interface
{
	std::string profile, bindings, error;
	std::vector<shader_parameter> inputs, outputs;
	bool dxil = false;
};
inline std::vector<std::string> shader_option_lines(const std::string &text)
{
	std::vector<std::string> lines; std::istringstream input(text); std::string line;
	while (std::getline(input, line))
	{
		const auto first = line.find_first_not_of(" \t\r");
		if (first != std::string::npos) lines.push_back(line.substr(first, line.find_last_not_of(" \t\r") - first + 1));
	}
	return lines;
}
inline bool shader_definitions(const std::string &text, std::vector<std::pair<std::string, std::string>> &definitions, std::string &error)
{
	for (const auto &line : shader_option_lines(text))
	{
		const auto equal = line.find('='); const auto name = line.substr(0, equal);
		if (name.empty() || (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_') ||
			std::any_of(name.begin(), name.end(), [](unsigned char c) { return !std::isalnum(c) && c != '_'; }))
		{ error = "Defines need one NAME or NAME=VALUE per line; macro names must be identifiers."; return false; }
		definitions.emplace_back(name, equal == std::string::npos ? "1" : line.substr(equal + 1));
	}
	return true;
}
class shader_include_files : public ID3DInclude
{
	struct opened_file { std::filesystem::path directory; std::vector<char> data; UINT size; };
	std::vector<std::filesystem::path> roots;
	std::map<const void *, opened_file> opened;
	size_t retained = 0;
public:
	shader_include_files(const std::filesystem::path &source, const std::vector<std::filesystem::path> &directories) : roots(directories)
	{ roots.insert(roots.begin(), source.has_parent_path() ? source.parent_path() : std::filesystem::path(".")); }
	HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR filename, LPCVOID parent, LPCVOID *data, UINT *size) override
	{
		try
		{
			auto search = roots;
			if (const auto it = opened.find(parent); it != opened.end()) search.insert(search.begin(), it->second.directory);
			for (const auto &root : search)
			{
				const auto path = root / std::filesystem::u8path(filename); std::ifstream file(path, std::ios::binary | std::ios::ate);
				if (!file) continue;
				const auto length = file.tellg(); if (length < 0 || length > 8 * 1024 * 1024 || retained + size_t(length) > 16 * 1024 * 1024) return E_FAIL;
				opened_file entry {path.parent_path(), std::vector<char>(std::max(size_t(1), size_t(length))), static_cast<UINT>(length)};
				file.seekg(0); if (length && !file.read(entry.data.data(), static_cast<std::streamsize>(length))) return E_FAIL;
				*size = entry.size; *data = entry.data.data(); retained += entry.size; opened.emplace(*data, std::move(entry)); return S_OK;
			}
			return E_FAIL;
		}
		catch (...) { return E_FAIL; }
	}
	HRESULT STDMETHODCALLTYPE Close(LPCVOID data) override
	{
		const auto it = opened.find(data); if (it == opened.end()) return E_INVALIDARG;
		retained -= it->second.size; opened.erase(it); return S_OK;
	}
};
template <typename Reflection, typename Desc, typename Parameter, typename Binding>
inline bool describe_shader(Reflection *reflection, shader_interface &info)
{
	Desc desc {};
	if (FAILED(reflection->GetDesc(&desc))) return false;
	const unsigned stage = desc.Version >> 16;
	const char *stages[] = {"ps", "vs", "gs", "hs", "ds", "cs"};
	if (stage >= std::size(stages) || !stages[stage][0]) { info.error = "This shader stage needs an external authoring tool."; return false; }
	info.profile = std::string(stages[stage]) + "_" + std::to_string((desc.Version >> 4) & 15) + "_" + std::to_string(desc.Version & 15);
	for (bool output : {false, true})
	{
		auto &parameters = output ? info.outputs : info.inputs;
		for (UINT i = 0; i < (output ? desc.OutputParameters : desc.InputParameters); ++i)
		{
			Parameter param {};
			if (FAILED(output ? reflection->GetOutputParameterDesc(i, &param) : reflection->GetInputParameterDesc(i, &param))) return false;
			unsigned components = 0; for (unsigned mask = param.Mask; mask; mask >>= 1) components += mask & 1;
			if (!param.SemanticName || !components || components > 4 || param.ComponentType == D3D_REGISTER_COMPONENT_UNKNOWN) return false;
			std::string type = param.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32 ? "float" : param.ComponentType == D3D_REGISTER_COMPONENT_UINT32 ? "uint" : "int";
			if (components > 1) type += std::to_string(components);
			parameters.push_back({param.SemanticName, type, param.SemanticIndex, static_cast<uint32_t>(param.SystemValueType), param.ComponentType != D3D_REGISTER_COMPONENT_FLOAT32});
		}
	}
	std::ostringstream bindings;
	for (UINT i = 0; i < desc.BoundResources; ++i)
	{
		Binding binding {};
		if (FAILED(reflection->GetResourceBindingDesc(i, &binding))) continue;
		const bool uav = binding.Type == D3D_SIT_UAV_RWTYPED || binding.Type == D3D_SIT_UAV_RWSTRUCTURED || binding.Type == D3D_SIT_UAV_RWBYTEADDRESS ||
			binding.Type == D3D_SIT_UAV_APPEND_STRUCTURED || binding.Type == D3D_SIT_UAV_CONSUME_STRUCTURED || binding.Type == D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER ||
			static_cast<unsigned>(binding.Type) == 13; // D3D_SIT_UAV_FEEDBACKTEXTURE in newer SDKs.
		const char reg = binding.Type == D3D_SIT_CBUFFER ? 'b' : binding.Type == D3D_SIT_SAMPLER ? 's' : uav ? 'u' : 't';
		bindings << (binding.Name ? binding.Name : "unnamed") << " : " << reg << binding.BindPoint << " (count " << binding.BindCount << ")";
		if constexpr (std::is_same_v<Binding, D3D12_SHADER_INPUT_BIND_DESC>) bindings << ", space " << binding.Space;
		bindings << '\n';
	}
	info.bindings = bindings.str(); return true;
}
inline shader_interface inspect_shader(const void *code, size_t size)
{
	shader_interface info;
	shader_com<ID3D11ShaderReflection> dxbc;
	if (SUCCEEDED(D3DReflect(code, size, IID_ID3D11ShaderReflection, reinterpret_cast<void **>(&dxbc.ptr))) && dxbc.ptr)
	{
		if (describe_shader<ID3D11ShaderReflection, D3D11_SHADER_DESC, D3D11_SIGNATURE_PARAMETER_DESC, D3D11_SHADER_INPUT_BIND_DESC>(dxbc.ptr, info)) return info;
		info.profile.clear(); info.error = "Could not read the original shader interface."; return info;
	}
	const auto create = load_dxc(info.error);
	if (!create) return info;
	constexpr GUID utils_id {0x4605c4cb, 0x2019, 0x492a, {0xad, 0xa4, 0x65, 0xf2, 0x0b, 0xb7, 0xd6, 0x7f}};
	constexpr GUID container_id {0xd2c21b26, 0x8350, 0x4bdc, {0x97, 0x6a, 0x33, 0x1c, 0xe6, 0xf4, 0xc5, 0x4c}};
	constexpr GUID reflection_id {0x5a58797d, 0xa72c, 0x478d, {0x8b, 0xa2, 0xef, 0xc6, 0xb0, 0xef, 0xe8, 0x8e}};
	shader_com<IDxcUtils> utils; shader_com<IDxcContainerReflection> container;
	shader_com<IDxcBlobEncoding> blob; shader_com<ID3D12ShaderReflection> dxil;
	UINT32 part = 0;
	if (size > UINT32_MAX || FAILED(create(CLSID_DxcUtils, utils_id, reinterpret_cast<void **>(&utils.ptr))) ||
		FAILED(create(CLSID_DxcContainerReflection, container_id, reinterpret_cast<void **>(&container.ptr))) ||
		FAILED(utils->CreateBlobFromPinned(code, static_cast<UINT32>(size), DXC_CP_ACP, &blob.ptr)) ||
		FAILED(container->Load(blob.ptr)) || FAILED(container->FindFirstPartKind(0x4c495844, &part)) || // DXIL FourCC
		FAILED(container->GetPartReflection(part, reflection_id, reinterpret_cast<void **>(&dxil.ptr))) ||
		!describe_shader<ID3D12ShaderReflection, D3D12_SHADER_DESC, D3D12_SIGNATURE_PARAMETER_DESC, D3D12_SHADER_INPUT_BIND_DESC>(dxil.ptr, info))
	{ info.profile.clear(); info.error = "No usable Direct3D reflection data. Import compatible source/bytecode produced by an external tool."; return info; }
	info.dxil = true; info.error.clear(); return info;
}
inline std::string parameter_semantic(const shader_parameter &param)
{ return param.semantic + (param.index ? std::to_string(param.index) : ""); }
inline std::string pixel_shader_starter(const shader_interface &info, const std::string &entry)
{
	if (info.profile.substr(0, 3) != "ps_" || info.outputs.empty()) return {};
	for (const auto &param : info.outputs)
		if (param.system != D3D_NAME_TARGET && param.system != D3D_NAME_DEPTH && param.system != D3D_NAME_DEPTH_GREATER_EQUAL &&
			param.system != D3D_NAME_DEPTH_LESS_EQUAL && param.system != D3D_NAME_COVERAGE && param.semantic != "SV_StencilRef") return {};
	std::ostringstream source;
	source << "// Diagnostic replacement, generated from the original " << info.profile << " interface.\n"
		"// Outputs magenta; edit the color below. This does not preserve original lighting.\n"
		"// Original resource bindings (declarations must be supplied if you use them):\n";
	std::istringstream bindings(info.bindings); std::string line;
	while (std::getline(bindings, line)) source << "// " << line << '\n';
	if (!info.inputs.empty())
	{
		source << "\nstruct PSInput\n{\n";
		for (size_t i = 0; i < info.inputs.size(); ++i)
			source << "    " << (info.inputs[i].integer ? "nointerpolation " : "") << info.inputs[i].type << " input" << i << " : " << parameter_semantic(info.inputs[i]) << ";\n";
		source << "};\n";
	}
	source << "\nstruct PSOutput\n{\n";
	for (size_t i = 0; i < info.outputs.size(); ++i) source << "    " << info.outputs[i].type << " output" << i << " : " << parameter_semantic(info.outputs[i]) << ";\n";
	source << "};\n\nPSOutput " << (entry.empty() ? "main" : entry) << '(' << (info.inputs.empty() ? "" : "PSInput input") << ")\n{\n"
		"    PSOutput output = (PSOutput)0;\n    float4 color = float4(1.0, 0.0, 1.0, 1.0);\n";
	for (size_t i = 0; i < info.outputs.size(); ++i)
	{
		const auto &param = info.outputs[i];
		std::string value = param.integer ? "0" : "color";
		if (param.system == D3D_NAME_COVERAGE) value = "0xFFFFFFFFu";
		if (param.system == D3D_NAME_DEPTH || param.system == D3D_NAME_DEPTH_GREATER_EQUAL || param.system == D3D_NAME_DEPTH_LESS_EQUAL)
		{
			value = "0.5";
			for (size_t j = 0; j < info.inputs.size(); ++j) if (info.inputs[j].system == D3D_NAME_POSITION) value = "input.input" + std::to_string(j) + ".z";
		}
		source << "    output.output" << i << " = (" << param.type << ")" << value << ";\n";
	}
	source << "    return output;\n}\n"; return source.str();
}
inline bool compile_dxil_source(const std::string &source, const std::filesystem::path &path, const std::string &entry,
	const std::string &profile, std::vector<uint8_t> &code, std::string &diagnostic,
	const std::vector<std::filesystem::path> &directories = {}, const std::vector<std::pair<std::string, std::string>> &definitions = {})
{
	const auto create = load_dxc(diagnostic);
	if (!create) return false;
	static HMODULE validator = nullptr;
	if (!validator) validator = LoadLibraryExW((dxc_library_path().parent_path() / "dxil.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!validator) { diagnostic = "DXIL compilation requires ModKit-tools/dxil.dll. Update the bundled tools folder."; return false; }
	constexpr GUID compiler_id {0x228b4687, 0x5a6a, 0x4730, {0x90, 0x0c, 0x97, 0x02, 0xb2, 0x20, 0x3f, 0x54}};
	constexpr GUID utils_id {0x4605c4cb, 0x2019, 0x492a, {0xad, 0xa4, 0x65, 0xf2, 0x0b, 0xb7, 0xd6, 0x7f}};
	constexpr GUID result_id {0x58346cda, 0xdde7, 0x4497, {0x94, 0x61, 0x6f, 0x87, 0xaf, 0x5e, 0x06, 0x59}};
	shader_com<IDxcCompiler3> compiler; shader_com<IDxcUtils> utils; shader_com<IDxcIncludeHandler> includes;
	shader_com<IDxcResult> result; shader_com<IDxcBlob> object;
	if (FAILED(create(CLSID_DxcCompiler, compiler_id, reinterpret_cast<void **>(&compiler.ptr))) ||
		FAILED(create(CLSID_DxcUtils, utils_id, reinterpret_cast<void **>(&utils.ptr))) || FAILED(utils->CreateDefaultIncludeHandler(&includes.ptr)))
	{ diagnostic = "Could not initialize the DXIL compiler."; return false; }
	const std::wstring filename = path.wstring(), include_dir = path.has_parent_path() ? path.parent_path().wstring() : L".", target(profile.begin(), profile.end()), function(entry.begin(), entry.end());
	std::vector<std::wstring> arguments {filename, L"-E", function, L"-T", target, L"-I", include_dir, L"-O3", L"-Ges"};
	for (const auto &directory : directories) { arguments.push_back(L"-I"); arguments.push_back(directory.wstring()); }
	for (const auto &definition : definitions) { arguments.push_back(L"-D"); arguments.push_back(std::filesystem::u8path(definition.first + "=" + definition.second).wstring()); }
	std::vector<const wchar_t *> args; for (const auto &argument : arguments) args.push_back(argument.c_str());
	DxcBuffer buffer {source.data(), source.size(), DXC_CP_UTF8};
	if (FAILED(compiler->Compile(&buffer, args.data(), static_cast<UINT32>(args.size()), includes.ptr, result_id, reinterpret_cast<void **>(&result.ptr))) || !result.ptr)
	{ diagnostic = "DXC could not compile this source."; return false; }
	shader_com<IDxcBlobEncoding> errors;
	if (SUCCEEDED(result->GetErrorBuffer(&errors.ptr)) && errors.ptr && errors->GetBufferSize())
		diagnostic.assign(static_cast<const char *>(errors->GetBufferPointer()), std::min<size_t>(errors->GetBufferSize(), 65536));
	while (!diagnostic.empty() && diagnostic.back() == '\0') diagnostic.pop_back();
	HRESULT status = E_FAIL;
	if (FAILED(result->GetStatus(&status)) || FAILED(status) || FAILED(result->GetResult(&object.ptr)) || !object.ptr)
	{ if (diagnostic.empty()) diagnostic = "DXIL compilation failed. Check source and that ModKit-tools includes dxil.dll."; return false; }
	const auto bytes = static_cast<const uint8_t *>(object->GetBufferPointer());
	code.assign(bytes, bytes + object->GetBufferSize()); return true;
}
}
