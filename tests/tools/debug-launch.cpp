//----------------------------------------------------------------------------
//	デバッガとしてプログラムを起動し、例外の発生位置をモジュール名で報告する
//
//	  debug-launch <exe> [作業ディレクトリ]
//
//	Visual Studio や WinDbg を入れずに「どこで落ちたか」を特定する為の物。
//	レジストリ (WER の LocalDumps) も触らない。
//
//	**アンロード済みモジュールの範囲も覚えておく**のが要点。
//	AviUtl2 の終了時クラッシュは「アンロードしたプラグインのコードを
//	実行しようとしていた」事が原因だったが、これは WER のモジュール一覧
//	(読み込み中の物しか出ない) からは判らず、この方法で初めて判明した。
//	詳細は docs/development.md の「終了時のクラッシュ」を参照。
//
//	出力例:
//	  [unload] ...\TSMemory-TVTestSrc.aux2
//	  [exception] code=0xC0000005 addr=00007FFF59BB1BD0 first=1 thread=33040
//	      -> ★アンロード済み ...\TSMemory-TVTestSrc.aux2 + 0x1BD0
//----------------------------------------------------------------------------
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct Module {
	ULONG_PTR Base;
	ULONG_PTR Size;
	std::wstring Name;
	bool fLoaded;
};

std::vector<Module> g_Modules;

//	デバッグ対象の PE ヘッダを読んで実際のイメージサイズを得る
//	(取れない場合は 16MB と見なす)
ULONG_PTR GetImageSize(HANDLE hProcess, LPVOID pBase)
{
	BYTE Header[0x400] = {};
	SIZE_T Read = 0;

	if (!::ReadProcessMemory(hProcess, pBase, Header, sizeof(Header), &Read)
			|| Read < sizeof(Header)) {
		return 0x1000000;
	}

	const IMAGE_DOS_HEADER *pDos = reinterpret_cast<const IMAGE_DOS_HEADER *>(Header);
	if (pDos->e_magic != IMAGE_DOS_SIGNATURE
			|| pDos->e_lfanew <= 0
			|| static_cast<size_t>(pDos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > sizeof(Header)) {
		return 0x1000000;
	}

	const IMAGE_NT_HEADERS64 *pNt =
		reinterpret_cast<const IMAGE_NT_HEADERS64 *>(Header + pDos->e_lfanew);
	if (pNt->Signature != IMAGE_NT_SIGNATURE || pNt->OptionalHeader.SizeOfImage == 0)
		return 0x1000000;

	return pNt->OptionalHeader.SizeOfImage;
}

std::wstring GetModuleName(HANDLE hFile, LPVOID pBase)
{
	WCHAR szPath[MAX_PATH] = L"";

	if (hFile != nullptr
			&& ::GetFinalPathNameByHandleW(hFile, szPath, MAX_PATH, FILE_NAME_NORMALIZED) == 0) {
		szPath[0] = L'\0';
	}
	if (szPath[0] == L'\0')
		::wsprintfW(szPath, L"<base %p>", pBase);

	return szPath;
}

void ReportAddress(ULONG_PTR Address)
{
	//	読み込み中の物を先に見る
	for (const Module &m : g_Modules) {
		if (m.fLoaded && Address >= m.Base && Address < m.Base + m.Size) {
			::wprintf(L"    -> %s + 0x%llX\n", m.Name.c_str(),
					  static_cast<unsigned long long>(Address - m.Base));
			return;
		}
	}
	//	アンロード済みの範囲に当たるならそれが原因
	for (const Module &m : g_Modules) {
		if (!m.fLoaded && Address >= m.Base && Address < m.Base + m.Size) {
			::wprintf(L"    -> ★アンロード済み %s + 0x%llX\n", m.Name.c_str(),
					  static_cast<unsigned long long>(Address - m.Base));
			return;
		}
	}
	::wprintf(L"    -> どのモジュールにも該当せず (解放済みメモリ)\n");
}

}	// namespace

int wmain(int argc, wchar_t **argv)
{
	if (argc < 2) {
		::wprintf(L"usage: debug-launch <exe> [作業ディレクトリ]\n");
		return 1;
	}

	::setvbuf(stdout, nullptr, _IONBF, 0);

	STARTUPINFOW si = {};
	PROCESS_INFORMATION pi = {};
	si.cb = sizeof(si);

	if (!::CreateProcessW(argv[1], nullptr, nullptr, nullptr, FALSE,
						  DEBUG_ONLY_THIS_PROCESS, nullptr,
						  argc > 2 ? argv[2] : nullptr, &si, &pi)) {
		::wprintf(L"CreateProcess failed: %u\n", ::GetLastError());
		return 1;
	}
	::wprintf(L"debugging %s (pid=%u)\n", argv[1], pi.dwProcessId);

	HANDLE hProcess = nullptr;
	bool fFirstBreakpoint = true;

	for (;;) {
		DEBUG_EVENT ev = {};
		if (!::WaitForDebugEvent(&ev, 300000)) {
			::wprintf(L"[timeout] デバッグイベントが来ません\n");
			break;
		}

		DWORD Continue = DBG_CONTINUE;

		switch (ev.dwDebugEventCode) {
		case CREATE_PROCESS_DEBUG_EVENT: {
			hProcess = ev.u.CreateProcessInfo.hProcess;
			Module m;
			m.Base = reinterpret_cast<ULONG_PTR>(ev.u.CreateProcessInfo.lpBaseOfImage);
			m.Size = GetImageSize(hProcess, ev.u.CreateProcessInfo.lpBaseOfImage);
			m.Name = GetModuleName(ev.u.CreateProcessInfo.hFile,
								   ev.u.CreateProcessInfo.lpBaseOfImage);
			m.fLoaded = true;
			g_Modules.push_back(m);
			if (ev.u.CreateProcessInfo.hFile != nullptr)
				::CloseHandle(ev.u.CreateProcessInfo.hFile);
			break;
		}

		case LOAD_DLL_DEBUG_EVENT: {
			Module m;
			m.Base = reinterpret_cast<ULONG_PTR>(ev.u.LoadDll.lpBaseOfDll);
			m.Size = GetImageSize(hProcess, ev.u.LoadDll.lpBaseOfDll);
			m.Name = GetModuleName(ev.u.LoadDll.hFile, ev.u.LoadDll.lpBaseOfDll);
			m.fLoaded = true;
			g_Modules.push_back(m);
			if (ev.u.LoadDll.hFile != nullptr)
				::CloseHandle(ev.u.LoadDll.hFile);
			break;
		}

		case UNLOAD_DLL_DEBUG_EVENT: {
			const ULONG_PTR Base = reinterpret_cast<ULONG_PTR>(ev.u.UnloadDll.lpBaseOfDll);
			for (Module &m : g_Modules) {
				if (m.Base == Base && m.fLoaded) {
					m.fLoaded = false;
					::wprintf(L"[unload] %s\n", m.Name.c_str());
				}
			}
			break;
		}

		case EXCEPTION_DEBUG_EVENT: {
			const EXCEPTION_RECORD &er = ev.u.Exception.ExceptionRecord;

			//	起動時の最初のブレークポイントはデバッガへの合図なので黙って通す
			if (er.ExceptionCode == EXCEPTION_BREAKPOINT && fFirstBreakpoint) {
				fFirstBreakpoint = false;
				break;
			}

			::wprintf(L"[exception] code=0x%08X addr=%p first=%d thread=%u\n",
					  er.ExceptionCode, er.ExceptionAddress,
					  ev.u.Exception.dwFirstChance, ev.dwThreadId);
			ReportAddress(reinterpret_cast<ULONG_PTR>(er.ExceptionAddress));

			if (er.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er.NumberParameters >= 2) {
				::wprintf(L"    %s address %p\n",
						  er.ExceptionInformation[0] != 0 ? L"write to" : L"read from",
						  reinterpret_cast<void *>(er.ExceptionInformation[1]));
			}

			//	こちらでは処理しない (対象のハンドラに任せる)
			Continue = DBG_EXCEPTION_NOT_HANDLED;
			break;
		}

		case EXIT_PROCESS_DEBUG_EVENT:
			::wprintf(L"[exit] code=0x%08X\n", ev.u.ExitProcess.dwExitCode);
			::ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
			return ev.u.ExitProcess.dwExitCode == 0 ? 0 : 1;
		}

		::ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, Continue);
	}

	return 0;
}
