//----------------------------------------------------------------------------
//	UTF-8 の設定ファイルから日本語の値が読めるかを確認する。
//
//	GetPrivateProfileStringW() は BOM の無いファイルを ANSI (CP932) として
//	読む為、UTF-8 の TSMemory-TVTestSrc.ini に書いた「キャプチャ用フィルタ」の様な値は
//	そのままでは文字化けする。TSMemoryGetIniString() はこれを避ける為の物。
//----------------------------------------------------------------------------
#include <windows.h>
#include <shlwapi.h>
#include <cstdio>

#include "inifile.h"

namespace {

int g_failures = 0;

void check(const char *what, bool ok)
{
	std::printf("%-56s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		g_failures++;
}

bool WriteAll(LPCWSTR path, const void *data, size_t size)
{
	HANDLE hFile = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
								 FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
		return false;
	DWORD Written = 0;
	const BOOL fOK = ::WriteFile(hFile, data, static_cast<DWORD>(size), &Written, nullptr);
	::CloseHandle(hFile);
	return fOK != FALSE;
}

//	「キャプチャ用フィルタ」
const WCHAR PRESET_NAME[] = L"\u30AD\u30E3\u30D7\u30C1\u30E3\u7528\u30D5\u30A3\u30EB\u30BF";

WCHAR g_szDir[MAX_PATH];

LPCWSTR MakeIni(LPCWSTR name, const void *data, size_t size)
{
	static WCHAR szPath[MAX_PATH];
	::lstrcpynW(szPath, g_szDir, MAX_PATH);
	::PathAppendW(szPath, name);
	if (!WriteAll(szPath, data, size))
		return L"";
	return szPath;
}

}	// namespace

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	if (argc > 1) {
		::MultiByteToWideChar(CP_ACP, 0, argv[1], -1, g_szDir, MAX_PATH);
	} else {
		::GetTempPathW(MAX_PATH, g_szDir);
		::PathAppendW(g_szDir, L"tsmemory_ini_test");
	}
	::CreateDirectoryW(g_szDir, nullptr);

	WCHAR szValue[128];

	//	--- UTF-8 (BOM 無し) --------------------------------------------------
	{
		const char Ini[] =
			"; comment\r\n"
			"[Capture]\r\n"
			"Format=png\r\n"
			"[Bridge]\r\n"
			"Layer=3\r\n"
			"Preset=\xE3\x82\xAD\xE3\x83\xA3\xE3\x83\x97\xE3\x83\x81\xE3\x83\xA3"
			"\xE7\x94\xA8\xE3\x83\x95\xE3\x82\xA3\xE3\x83\xAB\xE3\x82\xBF\r\n";
		LPCWSTR ini = MakeIni(L"utf8.ini", Ini, sizeof(Ini) - 1);

		TSMemoryGetIniString(ini, L"Bridge", L"Preset", L"", szValue, 128);
		check("UTF-8 without BOM : a Japanese value is read correctly",
			  ::lstrcmpW(szValue, PRESET_NAME) == 0);

		//	比較用: Win32 の物は文字化けする (この差がこの関数の存在理由)
		WCHAR szWin32[128] = {};
		::GetPrivateProfileStringW(L"Bridge", L"Preset", L"", szWin32, 128, ini);
		check("  (GetPrivateProfileStringW would mangle it)",
			  ::lstrcmpW(szWin32, PRESET_NAME) != 0);

		TSMemoryGetIniString(ini, L"Capture", L"Format", L"", szValue, 128);
		check("the right section is used", ::lstrcmpW(szValue, L"png") == 0);

		TSMemoryGetIniString(ini, L"bridge", L"preset", L"", szValue, 128);
		check("section and key names are case insensitive",
			  ::lstrcmpW(szValue, PRESET_NAME) == 0);

		TSMemoryGetIniString(ini, L"Bridge", L"NotThere", L"fallback", szValue, 128);
		check("a missing key falls back to the default",
			  ::lstrcmpW(szValue, L"fallback") == 0);

		TSMemoryGetIniString(ini, L"NoSuchSection", L"Preset", L"fallback", szValue, 128);
		check("a missing section falls back to the default",
			  ::lstrcmpW(szValue, L"fallback") == 0);
	}

	//	--- UTF-8 (BOM 付き) --------------------------------------------------
	{
		const char Ini[] =
			"\xEF\xBB\xBF[Bridge]\r\n"
			"Preset=\xE3\x82\xAD\xE3\x83\xA3\xE3\x83\x97\xE3\x83\x81\xE3\x83\xA3"
			"\xE7\x94\xA8\xE3\x83\x95\xE3\x82\xA3\xE3\x83\xAB\xE3\x82\xBF\r\n";
		LPCWSTR ini = MakeIni(L"utf8bom.ini", Ini, sizeof(Ini) - 1);

		TSMemoryGetIniString(ini, L"Bridge", L"Preset", L"", szValue, 128);
		check("UTF-8 with BOM : a Japanese value is read correctly",
			  ::lstrcmpW(szValue, PRESET_NAME) == 0);
	}

	//	--- CP932 (古い設定ファイル) は Win32 に任せる ------------------------
	{
		char Ansi[128] = "[Bridge]\r\nPreset=";
		char szName[64] = {};
		::WideCharToMultiByte(932, 0, PRESET_NAME, -1, szName, 64, nullptr, nullptr);
		::lstrcatA(Ansi, szName);
		::lstrcatA(Ansi, "\r\n");
		LPCWSTR ini = MakeIni(L"cp932.ini", Ansi, ::lstrlenA(Ansi));

		TSMemoryGetIniString(ini, L"Bridge", L"Preset", L"", szValue, 128);
		check("a CP932 file still works (falls back to Win32)",
			  ::lstrcmpW(szValue, PRESET_NAME) == 0);
	}

	//	--- UTF-16 は Win32 に任せる ------------------------------------------
	{
		WCHAR Ini[128] = L"\uFEFF[Bridge]\r\nPreset=";
		::lstrcatW(Ini, PRESET_NAME);
		::lstrcatW(Ini, L"\r\n");
		LPCWSTR ini = MakeIni(L"utf16.ini", Ini,
							  static_cast<size_t>(::lstrlenW(Ini)) * sizeof(WCHAR));

		TSMemoryGetIniString(ini, L"Bridge", L"Preset", L"", szValue, 128);
		check("a UTF-16 file still works (falls back to Win32)",
			  ::lstrcmpW(szValue, PRESET_NAME) == 0);
	}

	//	--- 引用符と余白 ------------------------------------------------------
	{
		const char Ini[] =
			"[Capture]\r\n"
			"  FileName  =  \"C:\\pic\\cap\"  \r\n";
		LPCWSTR ini = MakeIni(L"quoted.ini", Ini, sizeof(Ini) - 1);

		TSMemoryGetIniString(ini, L"Capture", L"FileName", L"", szValue, 128);
		check("surrounding spaces and quotes are stripped",
			  ::lstrcmpW(szValue, L"C:\\pic\\cap") == 0);
	}

	//	--- 受け側に収まらない値 ----------------------------------------------
	//
	//	out へ直接 MultiByteToWideChar() すると、収まらない時に 0 が返って
	//	値が丸ごと空になっていた。GetPrivateProfileString() は切り詰めるので
	//	そちらに合わせる。
	{
		const char Ini[] =
			"[Bridge]\r\n"
			"Preset=abcdefghij\r\n";
		LPCWSTR ini = MakeIni(L"toolong.ini", Ini, sizeof(Ini) - 1);
		WCHAR szSmall[6];

		TSMemoryGetIniString(ini, L"Bridge", L"Preset", L"", szSmall, 6);
		check("a value too long for the buffer is truncated, not dropped",
			  ::lstrcmpW(szSmall, L"abcde") == 0);
	}

	//	--- 無い設定ファイル --------------------------------------------------
	{
		WCHAR szPath[MAX_PATH];
		::lstrcpynW(szPath, g_szDir, MAX_PATH);
		::PathAppendW(szPath, L"nosuch.ini");
		TSMemoryGetIniString(szPath, L"Bridge", L"Preset", L"fallback", szValue, 128);
		check("a missing file falls back to the default",
			  ::lstrcmpW(szValue, L"fallback") == 0);
	}

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
				g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
