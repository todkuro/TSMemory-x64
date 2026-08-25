//----------------------------------------------------------------------------
//	tests/test_fuzz.cpp が見つけたクラッシュの位置を特定する
//
//	  fuzz-locate <ts-file> <aux2-path> <seed> <iteration>
//
//	test_fuzz と同じ順に乱数を消費して指定回目の壊し方を再現し、
//	入力プラグインに通す。落ちたら例外の番地と、スタックに残っている
//	「プラグインの中を指す戻り番地」を出力する。
//
//	番地は aux2 の先頭からのオフセット。llvm-symbolizer に食わせて
//	関数名と行に直す (tests/tools/fuzz-locate.sh がまとめて行う)。
//
//	※ 記号化する為には aux2 を -g 付きでビルドしておく必要がある。
//----------------------------------------------------------------------------
#include <windows.h>
#include <shlwapi.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "plugin2.h"
#include "input2.h"

namespace {

INPUT_PLUGIN_TABLE *g_pInput = nullptr;
EDIT_HANDLE g_Edit = {};
HOST_APP_TABLE g_Host = {};

void reg_input(INPUT_PLUGIN_TABLE *t) { g_pInput = t; }
void reg_window(LPCWSTR, HWND) {}
void reg_project(void (*)(PROJECT_FILE *)) {}
void reg_export(LPCWSTR, void *, void (*)(void *)) {}
EDIT_HANDLE *make_edit() { return &g_Edit; }

ULONG_PTR g_base = 0, g_end = 0;
volatile LONG g_reported = 0;

LONG CALLBACK OnException(EXCEPTION_POINTERS *ep)
{
	const DWORD code = ep->ExceptionRecord->ExceptionCode;

	if (code != EXCEPTION_ACCESS_VIOLATION
			&& code != EXCEPTION_INT_DIVIDE_BY_ZERO
			&& code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED
			&& code != EXCEPTION_STACK_OVERFLOW)
		return EXCEPTION_CONTINUE_SEARCH;

	//	最初の 1 件だけ出す (複数スレッドが同時に落ちる事がある)
	if (::InterlockedExchange(&g_reported, 1) != 0)
		return EXCEPTION_CONTINUE_SEARCH;

	const ULONG_PTR rip = reinterpret_cast<ULONG_PTR>(ep->ExceptionRecord->ExceptionAddress);

	std::printf("\n[exception] code=0x%08lX addr=%016llX\n",
				code, static_cast<unsigned long long>(rip));
	if (ep->ExceptionRecord->NumberParameters >= 2) {
		std::printf("            %s at %016llX\n",
					ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
					static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1]));
	}

	if (rip >= g_base && rip < g_end) {
		std::printf("FAULT %llX\n", static_cast<unsigned long long>(rip - g_base));
	} else {
		HMODULE hMod = nullptr;
		char szName[MAX_PATH] = "?";
		if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
								 | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
								 reinterpret_cast<LPCSTR>(rip), &hMod)) {
			::GetModuleFileNameA(hMod, szName, MAX_PATH);
		}
		std::printf("            in %s + 0x%llX (プラグインの外)\n",
					::PathFindFileNameA(szName),
					static_cast<unsigned long long>(rip - reinterpret_cast<ULONG_PTR>(hMod)));
	}

	//	スタックに残る戻り番地から呼び出し元をたどる
	{
		const ULONG_PTR *sp = reinterpret_cast<const ULONG_PTR *>(ep->ContextRecord->Rsp);
		ULONG_PTR prev = 0;
		int found = 0;

		for (int i = 0; i < 1024 && found < 16; i++) {
			if (::IsBadReadPtr(&sp[i], sizeof(ULONG_PTR)))
				break;
			const ULONG_PTR v = sp[i];
			if (v >= g_base && v < g_end && v != prev) {
				std::printf("CALLER %llX\n", static_cast<unsigned long long>(v - g_base));
				prev = v;
				found++;
			}
		}
	}

	::ExitProcess(2);
	return EXCEPTION_CONTINUE_SEARCH;
}

//	test_fuzz.cpp と同じ乱数・同じ壊し方 (変更する時は両方を合わせる事)
struct Rng {
	unsigned int s;
	explicit Rng(unsigned int seed) : s(seed != 0 ? seed : 1) {}
	unsigned int Next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
	unsigned int Below(unsigned int n) { return n != 0 ? Next() % n : 0; }
};

void Corrupt(std::vector<BYTE> *pTs, Rng *pRng, int Mode)
{
	const size_t Packets = pTs->size() / 188;
	if (Packets == 0)
		return;

	switch (Mode) {
	case 0:
		for (unsigned int i = 0; i < Packets; i++) {
			const size_t Offset = pRng->Below(static_cast<unsigned int>(pTs->size()));
			(*pTs)[Offset] ^= static_cast<BYTE>(1u << pRng->Below(8));
		}
		break;

	case 1:
		{
			std::vector<BYTE> Out;
			Out.reserve(pTs->size());
			for (size_t i = 0; i < Packets; i++) {
				if (pRng->Below(100) < 5)
					continue;
				Out.insert(Out.end(), pTs->begin() + static_cast<ptrdiff_t>(i * 188),
						   pTs->begin() + static_cast<ptrdiff_t>((i + 1) * 188));
			}
			pTs->swap(Out);
		}
		break;

	default:
		for (size_t i = 0; i < Packets; i++) {
			if (pRng->Below(100) < 10)
				(*pTs)[i * 188] = static_cast<BYTE>(pRng->Below(256));
		}
		break;
	}
}

bool Publish(const char *name, const std::vector<BYTE> &ts)
{
	SECURITY_DESCRIPTOR sd;
	SECURITY_ATTRIBUTES sa;

	::ZeroMemory(&sd, sizeof(sd));
	::InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
	::SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
	::ZeroMemory(&sa, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.lpSecurityDescriptor = &sd;

	char mutexName[MAX_PATH];
	::wnsprintfA(mutexName, MAX_PATH, "%s.mutex", name);
	::CreateMutexA(&sa, FALSE, mutexName);

	const DWORD total = static_cast<DWORD>(ts.size());
	HANDLE hMap = ::CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0,
									   sizeof(DWORD) * 4 + total, name);
	if (hMap == nullptr)
		return false;

	DWORD *p = static_cast<DWORD *>(::MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, 0));
	if (p == nullptr)
		return false;

	p[0] = total; p[1] = total; p[2] = 0; p[3] = 0;
	::CopyMemory(reinterpret_cast<BYTE *>(p) + sizeof(DWORD) * 4, ts.data(), total);
	return true;
}

}	// namespace


int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	if (argc < 5) {
		std::printf("usage: fuzz-locate <ts-file> <aux2-path> <seed> <iteration>\n");
		return 1;
	}

	::AddVectoredExceptionHandler(1, OnException);

	std::vector<BYTE> Original;
	{
		FILE *fp = std::fopen(argv[1], "rb");
		if (fp == nullptr) {
			std::printf("cannot open %s\n", argv[1]);
			return 1;
		}
		Original.resize(2 * 1024 * 1024 / 188 * 188);
		Original.resize(std::fread(Original.data(), 1, Original.size(), fp) / 188 * 188);
		std::fclose(fp);
	}

	const unsigned int Seed = static_cast<unsigned int>(std::atoi(argv[3]));
	const int Target = std::atoi(argv[4]);

	std::vector<BYTE> Ts;
	Rng rng(Seed);
	for (int it = 0; it <= Target; it++) {
		Ts = Original;
		Corrupt(&Ts, &rng, it % 3);
	}
	std::printf("seed %u iteration %d (mode %d) : %llu packets\n",
				Seed, Target, Target % 3,
				static_cast<unsigned long long>(Ts.size() / 188));

	if (!Publish("fuzzlocate.tvtv", Ts)) {
		std::printf("publish failed\n");
		return 1;
	}

	HMODULE hDll = ::LoadLibraryA(argv[2]);
	if (hDll == nullptr) {
		std::printf("LoadLibrary failed: %s\n", argv[2]);
		return 1;
	}
	{
		BYTE *p = reinterpret_cast<BYTE *>(hDll);
		IMAGE_DOS_HEADER *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(p);
		IMAGE_NT_HEADERS64 *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(p + dos->e_lfanew);
		g_base = reinterpret_cast<ULONG_PTR>(p);
		g_end = g_base + nt->OptionalHeader.SizeOfImage;
	}

	auto pInit = reinterpret_cast<bool (*)(DWORD)>(::GetProcAddress(hDll, "InitializePlugin"));
	auto pReg = reinterpret_cast<void (*)(HOST_APP_TABLE *)>(::GetProcAddress(hDll, "RegisterPlugin"));
	if (pInit == nullptr || pReg == nullptr) {
		std::printf("bad plugin\n");
		return 1;
	}

	g_Host.register_input_plugin = reg_input;
	g_Host.register_window_client = reg_window;
	g_Host.register_project_load_handler = reg_project;
	g_Host.register_export_menu_param = reg_export;
	g_Host.create_edit_handle = make_edit;

	pInit(0);
	pReg(&g_Host);
	if (g_pInput == nullptr) {
		std::printf("input plugin not registered\n");
		return 1;
	}

	WCHAR szPath[MAX_PATH];
	::GetCurrentDirectoryW(MAX_PATH, szPath);
	::lstrcatW(szPath, L"\\fuzzlocate.tvtv");

	std::printf("func_open ...\n");
	INPUT_HANDLE ih = g_pInput->func_open(szPath);
	std::printf("func_open -> %p\n", ih);

	if (ih != nullptr) {
		INPUT_INFO info = {};
		g_pInput->func_info_get(ih, &info);

		const int w = info.format != nullptr ? info.format->biWidth : 0;
		const int h = info.format != nullptr ? info.format->biHeight : 0;
		std::printf("%dx%d n=%d\n", w, h, info.n);

		if (w > 0 && h > 0 && w <= 16384 && h <= 16384) {
			const int pitch = ((16 * w + 31) & ~31) >> 3;
			std::vector<BYTE> buf(static_cast<size_t>(pitch) * h + 4096);
			for (int i = 0; i < 8 && i < info.n; i++) {
				std::printf("  read %d ...\n", i);
				g_pInput->func_read_video(ih, i, buf.data());
			}
		}
		g_pInput->func_close(ih);
	}

	std::printf("done (落ちませんでした)\n");
	return 0;
}
