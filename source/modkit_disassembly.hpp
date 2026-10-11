/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once
#include <dxcapi.h>
#include <algorithm>
#include <filesystem>
#include <string>

namespace reshade::modkit
{
static HMODULE dxc_module = nullptr;
inline std::filesystem::path dxc_library_path()
{
	HMODULE module = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&dxc_library_path), &module);
	wchar_t path[32768] = {};
	GetModuleFileNameW(module, path, 32768);
	return std::filesystem::path(path).parent_path() / "ModKit-tools" / "dxcompiler.dll";
}
inline DxcCreateInstanceProc load_dxc(std::string &error)
{
	if (!dxc_module)
		dxc_module = LoadLibraryExW(dxc_library_path().c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!dxc_module)
	{ error = "Install the bundled ModKit-tools folder beside the add-on. Expected DXC library: " + dxc_library_path().u8string(); return nullptr; }
	const auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dxc_module, "DxcCreateInstance"));
	if (!create) error = "The DXC library does not export DxcCreateInstance.";
	return create;
}
inline bool disassemble_dxil(const void *code, size_t size, std::string &text)
{
	const auto create = load_dxc(text);
	if (!create) return false;
	// Explicit SDK interface IDs also support MinGW, which does not implement
	// Microsoft's uuid attributes on the vendored DXC forward declarations.
	constexpr GUID compiler_id {0x228b4687, 0x5a6a, 0x4730, {0x90, 0x0c, 0x97, 0x02, 0xb2, 0x20, 0x3f, 0x54}};
	constexpr GUID result_id {0x58346cda, 0xdde7, 0x4497, {0x94, 0x61, 0x6f, 0x87, 0xaf, 0x5e, 0x06, 0x59}};
	constexpr GUID utf8_id {0x3da636c9, 0xba71, 0x4024, {0xa3, 0x01, 0x30, 0xcb, 0xf1, 0x25, 0x30, 0x5b}};
	IDxcCompiler3 *compiler = nullptr;
	if (FAILED(create(CLSID_DxcCompiler, compiler_id, reinterpret_cast<void **>(&compiler))))
	{ text = "Could not initialize the DXIL disassembler."; return false; }
	DxcBuffer buffer {code, size, DXC_CP_ACP};
	IDxcResult *result = nullptr;
	HRESULT status = compiler->Disassemble(&buffer, result_id, reinterpret_cast<void **>(&result));
	compiler->Release();
	if (FAILED(status) || !result) { if (result) result->Release(); text = "DXC could not read this shader container."; return false; }
	HRESULT operation_status = E_FAIL;
	status = result->GetStatus(&operation_status);
	IDxcBlobUtf8 *assembly = nullptr;
	bool success = SUCCEEDED(status) && SUCCEEDED(operation_status) && SUCCEEDED(result->GetOutput(DXC_OUT_DISASSEMBLY, utf8_id, reinterpret_cast<void **>(&assembly), nullptr)) && assembly;
	if (success)
	{
		if (assembly->GetStringLength() && assembly->GetStringLength() <= 8 * 1024 * 1024)
			text.assign(assembly->GetStringPointer(), assembly->GetStringLength());
		else { text = "DXIL disassembly is empty or exceeds the 8 MiB display limit."; success = false; }
	}
	else
	{
		IDxcBlobEncoding *errors = nullptr;
		if (SUCCEEDED(result->GetErrorBuffer(&errors)) && errors)
		{
			if (errors->GetBufferSize()) text.assign(static_cast<const char *>(errors->GetBufferPointer()), std::min<size_t>(errors->GetBufferSize(), 65536)); errors->Release();
		}
		if (text.empty()) text = "DXC disassembly failed for this shader container.";
	}
	if (assembly) assembly->Release();
	result->Release(); return success;
}
}
