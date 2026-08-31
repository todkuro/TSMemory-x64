//----------------------------------------------------------------------------
//	外字 (DRCS) の字形を貯めておく (drcs_store.h を参照)
//----------------------------------------------------------------------------
#include <windows.h>
#include <shlwapi.h>

#define STRSAFE_NO_DEPRECATE
#include <strsafe.h>

#include <map>
#include <string>
#include <vector>

#include "drcs_store.h"
#include "drcs_font.h"
#include "plugin_main.h"

namespace {

//	貯め込みの目印と版
const char STORE_MAGIC[8] = { 'T', 'S', 'M', 'D', 'R', 'C', 'S', '1' };

//	**枠の上限。**私用領域 (U+E000-U+F8FF) には余裕があるが、
//	フォントを作り直す手間と大きさが際限なく増えるのを防ぐ。
//	実測の字形は 36x36 の 4 階調 (2 ビット/画素) で 1 字形 324 バイト
//	なので、上限まで貯めても 166KB 程度
const int STORE_MAX = 512;

std::vector<TSMemoryDrcsGlyph> g_Glyphs;

//	中身 -> 枠の番号。**符号ではなく中身で引く**
std::map<std::string, int> g_Index;

//	本体が起動時に読み込んだ数。これより後ろは次の起動から使える
int g_LoadedCount = 0;

//	足した分があるか
bool g_fDirty = false;

WCHAR g_szStorePath[MAX_PATH] = {};

//	字形を 1 本の文字列にする (map の鍵)
std::string KeyOf(const TSMemoryDrcsGlyph &g)
{
	std::string s;
	s += static_cast<char>(g.Depth);
	s += static_cast<char>(g.Width);
	s += static_cast<char>(g.Height);
	s.append(reinterpret_cast<const char *>(g.Pattern.data()), g.Pattern.size());
	return s;
}

void Rebuild()
{
	g_Index.clear();
	for (size_t i = 0; i < g_Glyphs.size(); i++)
		g_Index[KeyOf(g_Glyphs[i])] = static_cast<int>(i);
}

bool WriteWholeFile(LPCWSTR pszPath, const BYTE *pData, size_t Size)
{
	HANDLE h = ::CreateFileW(pszPath, GENERIC_WRITE, 0, nullptr,
							 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	DWORD Written = 0;
	const BOOL fOK = ::WriteFile(h, pData, static_cast<DWORD>(Size),
								 &Written, nullptr);
	::CloseHandle(h);
	return fOK && Written == Size;
}

}	// namespace


void TSMemoryDrcsStoreLoad(LPCWSTR pszIniFile)
{
	g_Glyphs.clear();
	g_Index.clear();
	g_LoadedCount = 0;
	g_fDirty = false;

	//	ini と同じフォルダに置く (書ける事が判っている場所)
	::lstrcpynW(g_szStorePath, pszIniFile, MAX_PATH);
	::PathRemoveFileSpecW(g_szStorePath);
	::PathAppendW(g_szStorePath, L"TSMemoryDRCS.dat");

	HANDLE h = ::CreateFileW(g_szStorePath, GENERIC_READ, FILE_SHARE_READ,
							 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
							 nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return;			// まだ 1 つも貯めていない

	const DWORD Size = ::GetFileSize(h, nullptr);
	std::vector<BYTE> Data(Size);
	DWORD Read = 0;
	const BOOL fRead = ::ReadFile(h, Data.data(), Size, &Read, nullptr);
	::CloseHandle(h);
	if (!fRead || Read != Size || Size < sizeof(STORE_MAGIC) + 4)
		return;
	if (::memcmp(Data.data(), STORE_MAGIC, sizeof(STORE_MAGIC)) != 0)
		return;

	size_t p = sizeof(STORE_MAGIC);
	DWORD Count = 0;
	::CopyMemory(&Count, &Data[p], 4);
	p += 4;

	for (DWORD n = 0; n < Count && p + 8 <= Size; n++) {
		TSMemoryDrcsGlyph g;
		g.Depth = Data[p];
		g.Width = Data[p + 1];
		g.Height = Data[p + 2];
		p += 4;					// 1 バイトは詰め物
		DWORD Bytes = 0;
		::CopyMemory(&Bytes, &Data[p], 4);
		p += 4;
		if (p + Bytes > Size)
			break;
		g.Pattern.assign(Data.begin() + p, Data.begin() + p + Bytes);
		p += Bytes;
		g_Glyphs.push_back(g);
	}

	Rebuild();

	//	**読み込んだ分がフォントに入っている分。**
	//	本体も同じファイルから作った TTF を起動時に読んでいる
	g_LoadedCount = static_cast<int>(g_Glyphs.size());

	WCHAR sz[192];
	::StringCchPrintfW(sz, ARRAYSIZE(sz),
					   L"TSMemory: 外字の字形を %d 個読み込みました : %s",
					   g_LoadedCount, g_szStorePath);
	TSMemoryLog(sz);
}


int TSMemoryDrcsStoreLoadedCount()
{
	return g_LoadedCount;
}


int TSMemoryDrcsStoreFind(const TSMemoryDrcsGlyph &Glyph)
{
	auto it = g_Index.find(KeyOf(Glyph));
	return (it != g_Index.end()) ? it->second : -1;
}


int TSMemoryDrcsStoreAdd(const TSMemoryDrcsGlyph &Glyph)
{
	const int Found = TSMemoryDrcsStoreFind(Glyph);
	if (Found >= 0)
		return Found;
	if (static_cast<int>(g_Glyphs.size()) >= STORE_MAX)
		return -1;
	if (Glyph.Width == 0 || Glyph.Height == 0 || Glyph.Pattern.empty())
		return -1;			// TTF を作る所で 0 除算になる

	const int Slot = static_cast<int>(g_Glyphs.size());
	g_Glyphs.push_back(Glyph);
	g_Index[KeyOf(Glyph)] = Slot;
	g_fDirty = true;
	return Slot;
}


bool TSMemoryDrcsStoreRegisterFont(LPCWSTR pszFontName)
{
	if (g_Glyphs.empty() || pszFontName == nullptr || pszFontName[0] == L'\0')
		return false;

	//	**枠の番号がそのまま私用領域の符号になる。**
	//	取り込み側 (ts_caption.cpp) も同じ規則で本文を書き換える
	std::vector<TSMemoryDrcsGlyph> List = g_Glyphs;
	for (size_t i = 0; i < List.size(); i++)
		List[i].Code = static_cast<wchar_t>(0xE000 + i);

	std::vector<BYTE> Font;
	if (!TSMemoryBuildDrcsFont(List, pszFontName, &Font) || Font.empty()) {
		TSMemoryLogWarn(L"TSMemory: 外字のフォントを組み立てられませんでした");
		return false;
	}

	if (!TSMemoryRegisterFontCollection(Font.data(), Font.size())) {
		TSMemoryLogWarn(L"TSMemory: 外字のフォントを登録できませんでした");
		return false;
	}

	WCHAR sz[192];
	::StringCchPrintfW(sz, ARRAYSIZE(sz),
					   L"TSMemory: 外字のフォント「%s」を %d 字形で登録しました",
					   pszFontName, static_cast<int>(List.size()));
	TSMemoryLog(sz);
	return true;
}


bool TSMemoryDrcsStoreFlush()
{
	if (!g_fDirty || g_szStorePath[0] == L'\0')
		return false;
	g_fDirty = false;

	std::vector<BYTE> Out;
	Out.insert(Out.end(), STORE_MAGIC, STORE_MAGIC + sizeof(STORE_MAGIC));
	const DWORD Count = static_cast<DWORD>(g_Glyphs.size());
	Out.insert(Out.end(), reinterpret_cast<const BYTE *>(&Count),
			   reinterpret_cast<const BYTE *>(&Count) + 4);
	for (const TSMemoryDrcsGlyph &g : g_Glyphs) {
		Out.push_back(static_cast<BYTE>(g.Depth));
		Out.push_back(static_cast<BYTE>(g.Width));
		Out.push_back(static_cast<BYTE>(g.Height));
		Out.push_back(0);
		const DWORD Bytes = static_cast<DWORD>(g.Pattern.size());
		Out.insert(Out.end(), reinterpret_cast<const BYTE *>(&Bytes),
				   reinterpret_cast<const BYTE *>(&Bytes) + 4);
		Out.insert(Out.end(), g.Pattern.begin(), g.Pattern.end());
	}
	if (!WriteWholeFile(g_szStorePath, Out.data(), Out.size())) {
		TSMemoryLogWarn(L"TSMemory: 外字の字形を保存できませんでした");
		return false;
	}

	//	**フォントはここでは作らない。**
	//	本体は初期化の中でしか受け付けないので、次の起動の
	//	TSMemoryDrcsStoreRegisterFont() が この .dat から組み立てる
	WCHAR sz[MAX_PATH + 128];
	::StringCchPrintfW(sz, ARRAYSIZE(sz),
					   L"TSMemory: 外字の字形を保存しました "
					   L"(%d 字形 / うち %d 個は次の起動から使えます) : %s",
					   static_cast<int>(g_Glyphs.size()),
					   static_cast<int>(g_Glyphs.size()) - g_LoadedCount,
					   g_szStorePath);
	TSMemoryLog(sz);
	return true;
}
