//----------------------------------------------------------------------------
//	デコードのホットスポットをサンプリングで調べる
//
//	  m2v-profile <ts-file> <aux2-path> [frames]
//
//	TS を共有メモリに載せ、入力プラグイン (aux2) の func_open() /
//	func_read_video() で実際の経路のままデコードしながら、
//	全スレッドの RIP を一定間隔で採取する。
//
//	出力は「aux2 のイメージ先頭からのオフセット 出現数」。
//	llvm-symbolizer に食わせて関数名に直す
//	(tests/tools/m2v-profile.sh がまとめて行う)。
//
//	※ 記号化する為には aux2 を -g 付きでビルドしておく必要がある。
//----------------------------------------------------------------------------
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

#include "plugin2.h"
#include "input2.h"

namespace {

volatile LONG g_stop = 0;
std::map<ULONG_PTR, long> g_samples;
ULONG_PTR g_base = 0, g_end = 0;

INPUT_PLUGIN_TABLE *g_pInput = nullptr;
EDIT_HANDLE g_Edit = {};
HOST_APP_TABLE g_Host = {};

void fake_register_input_plugin(INPUT_PLUGIN_TABLE *t) { g_pInput = t; }
void fake_register_window_client(LPCWSTR, HWND) {}
void fake_register_project_load_handler(void (*)(PROJECT_FILE *)) {}
void fake_register_export_menu_param(LPCWSTR, void *, void (*)(void *)) {}
EDIT_HANDLE *fake_create_edit_handle() { return &g_Edit; }

DWORD WINAPI Sampler(LPVOID)
{
	const DWORD self = ::GetCurrentThreadId();
	const DWORD pid = ::GetCurrentProcessId();

	//	スレッドの列挙 (CreateToolhelp32Snapshot) は数十 ms 掛かるので
	//	毎回やると殆どサンプル出来ない。最初に一度だけ開いて使い回す。
	std::vector<HANDLE> threads;
	{
		HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap != INVALID_HANDLE_VALUE) {
			THREADENTRY32 te = {};
			te.dwSize = sizeof(te);
			if (::Thread32First(snap, &te)) {
				do {
					if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
						continue;
					HANDLE th = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
											 FALSE, te.th32ThreadID);
					if (th != nullptr)
						threads.push_back(th);
				} while (::Thread32Next(snap, &te));
			}
			::CloseHandle(snap);
		}
	}

	while (::InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
		for (HANDLE th : threads) {
			if (::SuspendThread(th) == static_cast<DWORD>(-1))
				continue;
			CONTEXT ctx = {};
			ctx.ContextFlags = CONTEXT_CONTROL;
			if (::GetThreadContext(th, &ctx)) {
				const ULONG_PTR rip = static_cast<ULONG_PTR>(ctx.Rip);
				if (rip >= g_base && rip < g_end)
					g_samples[rip]++;
			}
			::ResumeThread(th);
		}
		::Sleep(0);
	}

	for (HANDLE th : threads)
		::CloseHandle(th);
	return 0;
}

bool PublishTs(const char *name, const std::vector<BYTE> &ts)
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
	::wsprintfA(mutexName, "%s.mutex", name);
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

	if (argc < 3) {
		std::printf("usage: m2v-profile <ts-file> <aux2-path> [frames]\n");
		return 1;
	}
	const char *tsPath = argv[1];
	const char *dllPath = argv[2];
	const int frames = argc > 3 ? std::atoi(argv[3]) : 60;

	std::vector<BYTE> ts;
	{
		FILE *fp = std::fopen(tsPath, "rb");
		if (fp == nullptr) { std::printf("cannot open %s\n", tsPath); return 1; }
		std::fseek(fp, 0, SEEK_END);
		long size = std::ftell(fp);
		std::fseek(fp, 0, SEEK_SET);
		if (size > 20 * 1024 * 1024) size = 20 * 1024 * 1024;
		ts.resize(static_cast<size_t>(size));
		ts.resize(std::fread(ts.data(), 1, ts.size(), fp));
		std::fclose(fp);
	}

	const char *name = "m2vprof.tvtv";
	if (!PublishTs(name, ts)) { std::printf("PublishTs failed\n"); return 1; }
	{
		WCHAR w[MAX_PATH];
		::MultiByteToWideChar(CP_ACP, 0, name, -1, w, MAX_PATH);
		HANDLE h = ::CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
								 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
	}

	HMODULE hDll = ::LoadLibraryA(dllPath);
	if (hDll == nullptr) { std::printf("LoadLibrary failed: %s\n", dllPath); return 1; }
	{
		BYTE *p = reinterpret_cast<BYTE *>(hDll);
		IMAGE_DOS_HEADER *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(p);
		IMAGE_NT_HEADERS64 *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(p + dos->e_lfanew);
		g_base = reinterpret_cast<ULONG_PTR>(p);
		g_end = g_base + nt->OptionalHeader.SizeOfImage;
	}

	auto pInit = reinterpret_cast<bool (*)(DWORD)>(::GetProcAddress(hDll, "InitializePlugin"));
	auto pReg = reinterpret_cast<void (*)(HOST_APP_TABLE *)>(::GetProcAddress(hDll, "RegisterPlugin"));
	if (pInit == nullptr || pReg == nullptr) { std::printf("bad plugin\n"); return 1; }

	g_Host.register_input_plugin = fake_register_input_plugin;
	g_Host.register_window_client = fake_register_window_client;
	g_Host.register_project_load_handler = fake_register_project_load_handler;
	g_Host.register_export_menu_param = fake_register_export_menu_param;
	g_Host.create_edit_handle = fake_create_edit_handle;

	pInit(0);
	pReg(&g_Host);
	if (g_pInput == nullptr) { std::printf("input plugin not registered\n"); return 1; }

	WCHAR wpath[MAX_PATH];
	::GetCurrentDirectoryW(MAX_PATH, wpath);
	::lstrcatW(wpath, L"\\");
	{
		WCHAR w[MAX_PATH];
		::MultiByteToWideChar(CP_ACP, 0, name, -1, w, MAX_PATH);
		::lstrcatW(wpath, w);
	}

	INPUT_HANDLE ih = g_pInput->func_open(wpath);
	if (ih == nullptr) { std::printf("func_open failed\n"); return 1; }

	INPUT_INFO info = {};
	g_pInput->func_info_get(ih, &info);
	const int w = info.format != nullptr ? info.format->biWidth : 0;
	const int h = info.format != nullptr ? info.format->biHeight : 0;
	std::printf("decoding %dx%d  %d frames available\n", w, h, info.n);

	const int pitch = ((16 * w + 31) & ~31) >> 3;
	std::vector<BYTE> buf(static_cast<size_t>(pitch) * h + 64);

	HANDLE hs = ::CreateThread(nullptr, 0, Sampler, nullptr, 0, nullptr);

	const DWORD t0 = ::GetTickCount();
	int n = 0;
	for (int i = 0; i < frames && i < info.n; i++) {
		if (g_pInput->func_read_video(ih, i, buf.data()) > 0)
			n++;
	}
	const DWORD ms = ::GetTickCount() - t0;

	::InterlockedExchange(&g_stop, 1);
	::WaitForSingleObject(hs, 5000);
	::CloseHandle(hs);

	std::printf("decoded %d frames in %u ms (%.1f ms/frame)\n",
				n, ms, n > 0 ? static_cast<double>(ms) / n : 0.0);

	long total = 0;
	for (const auto &e : g_samples) total += e.second;
	std::printf("samples %ld\n", total);
	std::printf("--- RIP OFFSET HISTOGRAM ---\n");
	for (const auto &e : g_samples)
		std::printf("%llu %ld\n",
					static_cast<unsigned long long>(e.first - g_base), e.second);

	g_pInput->func_close(ih);
	return 0;
}
