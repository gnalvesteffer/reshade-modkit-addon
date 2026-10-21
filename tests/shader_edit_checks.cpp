/* SPDX-License-Identifier: BSD-3-Clause */
#include <windows.h>
#include "modkit_shader_edit.hpp"
#include <iostream>
#include <stdexcept>
#include <cstring>

using namespace reshade::modkit;
static void expect(bool condition, const std::string &message)
{ if (!condition) throw std::runtime_error(message); }
static std::vector<uint8_t> compile_dxbc(const std::string &source, const char *target = "ps_5_0",
	const std::vector<std::filesystem::path> &directories = {}, const std::vector<std::pair<std::string, std::string>> &definitions = {})
{
	shader_com<ID3DBlob> code, errors;
	std::vector<D3D_SHADER_MACRO> macros;
	for (const auto &definition : definitions) macros.push_back({definition.first.c_str(), definition.second.c_str()});
	macros.push_back({nullptr, nullptr}); shader_include_files includes("fixture.hlsl", directories);
	const HRESULT status = D3DCompile(source.data(), source.size(), "fixture.hlsl", macros.data(), &includes, "main", target,
		D3DCOMPILE_ENABLE_STRICTNESS, 0, &code.ptr, &errors.ptr);
	const std::string diagnostic = errors.ptr ? std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize()) : "";
	expect(SUCCEEDED(status) && code.ptr, "Fixture failed to compile: " + diagnostic);
	const auto bytes = static_cast<const uint8_t *>(code->GetBufferPointer()); return {bytes, bytes + code->GetBufferSize()};
}
struct include_directory
{
	std::filesystem::path path = std::filesystem::temp_directory_path() / ("modkit-includes-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
	include_directory() { std::filesystem::create_directories(path / "include folder/nested"); }
	~include_directory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
static void check_output_interface(const shader_interface &original, const shader_interface &replacement)
{
	expect(original.profile == replacement.profile, "Starter changed the shader profile");
	expect(original.outputs.size() == replacement.outputs.size(), "Starter lost an original output");
	for (const auto &output : original.outputs)
	{
		const auto match = std::find_if(replacement.outputs.begin(), replacement.outputs.end(), [&](const auto &param) {
			return param.semantic == output.semantic && param.index == output.index && param.type == output.type && param.system == output.system;
		});
		expect(match != replacement.outputs.end(), "Starter changed an output's type or semantic");
	}
}
int main()
{
	try
	{
		const std::string fixture = R"(
cbuffer Parameters : register(b3) { float4 tint; };
Texture2D<float4> albedo : register(t2);
SamplerState textureSampler : register(s1);
struct Input { float4 position : SV_Position; float2 uv : TEXCOORD1; nointerpolation uint objectId : TEXCOORD2; };
struct Output { float4 color : SV_Target0; float2 normal : SV_Target1; uint id : SV_Target2; float depth : SV_Depth; uint coverage : SV_Coverage; };
Output main(Input input) {
    Output output;
    output.color = albedo.Sample(textureSampler, input.uv) * tint;
    output.normal = input.uv;
    output.id = input.objectId;
    output.depth = input.position.z;
    output.coverage = 0xFFFFFFFFu;
    return output;
})";
		const auto original = compile_dxbc(fixture);
		const auto original_interface = inspect_shader(original.data(), original.size());
		expect(original_interface.profile == "ps_5_0" && !original_interface.dxil && original_interface.error.empty(), "DXBC interface unavailable: " + original_interface.error);
		expect(original_interface.inputs.size() == 3 && original_interface.outputs.size() == 5, "Expected input and output signatures");
		expect(original_interface.bindings.find("b3") != std::string::npos && original_interface.bindings.find("t2") != std::string::npos && original_interface.bindings.find("s1") != std::string::npos, "Resource registers missing from original interface");
		const auto starter = pixel_shader_starter(original_interface, "main");
		expect(!starter.empty(), "No generated pixel starter");
		const auto replacement = compile_dxbc(starter);
		check_output_interface(original_interface, inspect_shader(replacement.data(), replacement.size()));
		const auto no_inputs = compile_dxbc("float4 main() : SV_Target { return float4(1, 0, 0, 1); }");
		const auto no_input_interface = inspect_shader(no_inputs.data(), no_inputs.size());
		const auto no_input_starter = pixel_shader_starter(no_input_interface, "main");
		const auto no_input_replacement = compile_dxbc(no_input_starter);
		check_output_interface(no_input_interface, inspect_shader(no_input_replacement.data(), no_input_replacement.size()));
		shader_interface unsupported = original_interface; unsupported.profile = "vs_5_0";
		expect(pixel_shader_starter(unsupported, "main").empty(), "Generated a pixel starter for a vertex shader");
		unsupported = original_interface; unsupported.outputs[0].system = D3D_NAME_UNDEFINED;
		expect(pixel_shader_starter(unsupported, "main").empty(), "Generated unsupported output semantics");
		std::vector<uint8_t> dxil; std::string diagnostic;
		const bool compiled_dxil = compile_dxil_source(fixture, "fixture.hlsl", "main", "ps_6_0", dxil, diagnostic);
		expect(compiled_dxil, "DXIL fixture failed: " + diagnostic);
		const auto dxil_interface = inspect_shader(dxil.data(), dxil.size());
		expect(dxil_interface.profile == "ps_6_0" && dxil_interface.dxil && dxil_interface.error.empty(), "DXIL interface unavailable: " + dxil_interface.error);
		const auto dxil_starter = pixel_shader_starter(dxil_interface, "main");
		std::vector<uint8_t> dxil_replacement; diagnostic.clear();
		const bool compiled_starter = compile_dxil_source(dxil_starter, "starter.hlsl", "main", "ps_6_0", dxil_replacement, diagnostic);
		expect(compiled_starter, "DXIL starter failed: " + diagnostic);
		check_output_interface(dxil_interface, inspect_shader(dxil_replacement.data(), dxil_replacement.size()));
		std::vector<uint8_t> invalid; diagnostic.clear();
		expect(!compile_dxil_source("This is invalid HLSL", "invalid.hlsl", "main", "ps_6_0", invalid, diagnostic) && !diagnostic.empty(), "Invalid source did not report diagnostics");
		include_directory directory;
		{ std::ofstream file(directory.path / "include folder/api.hlsl"); file << "#include \"nested/color.hlsl\"\n"; }
		{ std::ofstream file(directory.path / "include folder/nested/color.hlsl"); file << "float4 tintColor() { return float4(0, INTENSITY, 0, 1); }\n"; }
		const std::string included_source = "#include \"api.hlsl\"\nfloat4 main() : SV_Target { return tintColor(); }\n";
		std::vector<std::pair<std::string, std::string>> definitions;
		expect(shader_definitions("INTENSITY=0.5\r\nFEATURE\n", definitions, diagnostic) && definitions.size() == 2 && definitions[1].second == "1", "Define parsing failed");
		const auto include_dxbc = compile_dxbc(included_source, "ps_5_0", {directory.path / "include folder"}, definitions);
		expect(inspect_shader(include_dxbc.data(), include_dxbc.size()).profile == "ps_5_0", "DXBC includes/macros failed");
		diagnostic.clear(); std::vector<uint8_t> include_dxil;
		const bool compiled_includes = compile_dxil_source(included_source, "includes.hlsl", "main", "ps_6_0", include_dxil, diagnostic, {directory.path / "include folder"}, definitions);
		expect(compiled_includes, "DXIL includes/macros failed: " + diagnostic);
		definitions.clear(); expect(!shader_definitions("BAD NAME=1", definitions, diagnostic), "Invalid macro identifier accepted");
		std::string profile_error;
		expect(!replacement_profile_ok(original_interface, dxil_interface, profile_error) && !profile_error.empty(), "Profile mismatch accepted (ps_5_0 vs ps_6_0)");
		expect(replacement_profile_ok(original_interface, original_interface, profile_error), "Same profile rejected");
		shader_interface same_profile_dxil = original_interface; same_profile_dxil.dxil = true;
		expect(!replacement_profile_ok(original_interface, same_profile_dxil, profile_error) && !profile_error.empty(), "Cross-format import accepted (ps_5_0 DXBC vs ps_5_0 DXIL)");
		shader_interface empty_original; expect(replacement_profile_ok(empty_original, dxil_interface, profile_error), "Empty original profile rejected");
		shader_interface empty_replacement; expect(replacement_profile_ok(original_interface, empty_replacement, profile_error), "Empty replacement profile rejected");
		std::cout << "Shader editor checks passed (DXBC and DXIL)\n"; return 0;
	}
	catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
