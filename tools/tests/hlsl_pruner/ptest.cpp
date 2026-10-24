#include <windows.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12HlslPruner.h"
using Microsoft::WRL::ComPtr;
static std::string ReadAll(const char* p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static bool Compile(IDxcCompiler3* c, const std::string& src, const wchar_t* profile, std::vector<char>& out, double& ms, std::string& err)
{
	DxcBuffer buf{ src.data(), src.size(), DXC_CP_UTF8 };
	LPCWSTR args[] = { L"-E", L"main", L"-T", profile, L"-HV", L"2021", L"-O3", L"-Qstrip_reflect", L"-Qstrip_debug" };
	ms = 1e9;
	for (int i = 0; i < 5; i++)
	{
		auto t0 = std::chrono::steady_clock::now();
		ComPtr<IDxcResult> res;
		c->Compile(&buf, args, 9, nullptr, IID_PPV_ARGS(&res));
		HRESULT st; res->GetStatus(&st);
		double t = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		ms = t < ms ? t : ms;
		if (FAILED(st)) { ComPtr<IDxcBlobUtf8> e; res->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&e), nullptr); err = e ? e->GetStringPointer() : "?"; return false; }
		ComPtr<IDxcBlob> o; res->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&o), nullptr);
		out.assign((char*)o->GetBufferPointer(), (char*)o->GetBufferPointer() + o->GetBufferSize());
	}
	return true;
}
int main(int argc, char** argv)
{
	ComPtr<IDxcCompiler3> compiler;
	DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
	int failures = 0;
	for (int a = 1; a < argc; a++)
	{
		const std::string name = argv[a];
		const wchar_t* profile = name.find("gs") == 0 ? L"gs_6_4" : L"ps_6_4";
		std::string src = ReadAll(argv[a]), pruned, err;
		const bool didPrune = D3D12HlslPruner::PruneUnusedFunctions(src, "main", pruned);
		std::vector<char> a1, a2; double t1, t2;
		if (!Compile(compiler.Get(), src, profile, a1, t1, err)) { printf("%s: original does not compile\n%s\n", argv[a], err.c_str()); failures++; continue; }
		if (!didPrune) { printf("%-14s nothing pruned, %.1f ms\n", argv[a], t1); continue; }
		if (!Compile(compiler.Get(), pruned, profile, a2, t2, err)) { printf("%s: PRUNED FAILS\n%s\n", argv[a], err.c_str()); failures++; continue; }
		const bool same = a1 == a2;
		if (!same) failures++;
		printf("%-14s %6zu -> %6zu chars, %.1f -> %.1f ms, dxil %s\n", argv[a], src.size(), pruned.size(), t1, t2, same ? "IDENTICAL" : "DIFFERENT");
	}
	return failures;
}
