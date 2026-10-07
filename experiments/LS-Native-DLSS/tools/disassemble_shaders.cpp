// Local research helper. Loads PE resources as data, without executing DllMain.
// Link with d3dcompiler.lib; proprietary disassembly stays in the user's local folder.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

struct Context { std::filesystem::path output; unsigned count = 0, errors = 0; };

BOOL CALLBACK Inspect(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR opaque) {
    auto& ctx = *reinterpret_cast<Context*>(opaque);
    if (!IS_INTRESOURCE(name)) return TRUE;
    auto resource = FindResourceW(module, name, type);
    auto size = SizeofResource(module, resource);
    auto handle = LoadResource(module, resource);
    auto data = static_cast<const char*>(LockResource(handle));
    if (!data || size < 4 || std::string(data, 4) != "DXBC") return TRUE;
    ID3DBlob* assembly = nullptr;
    auto hr = D3DDisassemble(data, size, D3D_DISASM_ENABLE_INSTRUCTION_NUMBERING, nullptr, &assembly);
    if (FAILED(hr) || !assembly) { ++ctx.errors; return TRUE; }
    auto id = reinterpret_cast<ULONG_PTR>(name);
    std::ofstream file(ctx.output / ("resource-" + std::to_string(id) + ".asm"), std::ios::binary);
    file.write(static_cast<const char*>(assembly->GetBufferPointer()), assembly->GetBufferSize());
    if (file) ++ctx.count; else ++ctx.errors;
    assembly->Release();
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { std::cerr << "Usage: disassemble_shaders.exe <native DLL> <local output folder>\n"; return 2; }
    Context ctx{std::filesystem::path(argv[2])};
    std::filesystem::create_directories(ctx.output);
    auto module = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) { std::cerr << "Cannot open DLL as resource data: " << GetLastError() << '\n'; return 1; }
    auto ok = EnumResourceNamesW(module, MAKEINTRESOURCEW(10), Inspect, reinterpret_cast<LONG_PTR>(&ctx));
    FreeLibrary(module);
    std::cout << ctx.count << " shader disassemblies, " << ctx.errors << " errors\n";
    return !ok || !ctx.count || ctx.errors ? 1 : 0;
}
