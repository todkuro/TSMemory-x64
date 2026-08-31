//----------------------------------------------------------------------------
//	外字 (DRCS) を本物の文字に置き換える (drcs_replace.h を参照)
//----------------------------------------------------------------------------
#include <windows.h>
#include <shlwapi.h>
#include <bcrypt.h>

#define STRSAFE_NO_DEPRECATE
#include <strsafe.h>

#include <map>
#include <string>
#include <vector>

#include "drcs_replace.h"
#include "plugin_main.h"

//	表の実体をここに置く
#define TSMEMORY_DRCS_MAP_DEFINE
#include "arib_drcs_map.h"

namespace {

//	利用者が足した分 (md5 の 16 バイト -> 文字)。
//	**組み込みの表より先に見る**ので、合わない字形を上書きできる
std::map<std::string, std::wstring> g_UserMap;

//	字形のビットマップの md5 を取る。
//
//	**幅・高さ・深さは含めない。**libaribcaption が生のビットマップだけを
//	対象にしているので、同じ取り方にしないと表が引けない
bool Md5Of(const std::vector<BYTE> &Data, BYTE *pOut16)
{
	BCRYPT_ALG_HANDLE hAlg = nullptr;
	if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_MD5_ALGORITHM,
													  nullptr, 0)))
		return false;

	BCRYPT_HASH_HANDLE hHash = nullptr;
	bool fOK = false;
	if (BCRYPT_SUCCESS(::BCryptCreateHash(hAlg, &hHash, nullptr, 0,
										  nullptr, 0, 0))) {
		fOK = BCRYPT_SUCCESS(::BCryptHashData(
				 hHash, const_cast<PUCHAR>(Data.data()),
				 static_cast<ULONG>(Data.size()), 0))
		   && BCRYPT_SUCCESS(::BCryptFinishHash(hHash, pOut16, 16, 0));
		::BCryptDestroyHash(hHash);
	}
	::BCryptCloseAlgorithmProvider(hAlg, 0);
	return fOK;
}

std::wstring ToHex(const BYTE *p)
{
	WCHAR sz[33];
	for (int i = 0; i < 16; i++)
		::StringCchPrintfW(sz + i * 2, 3, L"%02x", p[i]);
	return std::wstring(sz, 32);
}

//	UCS-4 を UTF-16 にする。𠮷 (U+20BB7) 等は 2 コード単位になる
std::wstring FromCodePoint(DWORD Code)
{
	std::wstring s;
	if (Code >= 0x10000 && Code <= 0x10FFFF) {
		const DWORD v = Code - 0x10000;
		s += static_cast<wchar_t>(0xD800 + (v >> 10));
		s += static_cast<wchar_t>(0xDC00 + (v & 0x3FF));
	} else if (Code != 0) {
		s += static_cast<wchar_t>(Code);
	}
	return s;
}

//	16 進 32 桁を 16 バイトにする
bool FromHex(const std::wstring &s, std::string *pOut)
{
	if (s.size() != 32)
		return false;
	pOut->clear();
	for (size_t i = 0; i < 32; i += 2) {
		int v = 0;
		for (int k = 0; k < 2; k++) {
			const wchar_t c = s[i + k];
			int d;
			if (c >= L'0' && c <= L'9')		d = c - L'0';
			else if (c >= L'a' && c <= L'f')	d = c - L'a' + 10;
			else if (c >= L'A' && c <= L'F')	d = c - L'A' + 10;
			else return false;
			v = v * 16 + d;
		}
		*pOut += static_cast<char>(v);
	}
	return true;
}

}	// namespace


int TSMemoryDrcsReplaceLoad(LPCWSTR pszIniFile)
{
	g_UserMap.clear();

	WCHAR szPath[MAX_PATH];
	::lstrcpynW(szPath, pszIniFile, MAX_PATH);
	::PathRemoveFileSpecW(szPath);
	::PathAppendW(szPath, L"TSMemoryDrcsMap.txt");

	HANDLE h = ::CreateFileW(szPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
							 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return 0;			// 無くて良い

	const DWORD Size = ::GetFileSize(h, nullptr);
	std::vector<char> Raw(Size + 1, 0);
	DWORD Read = 0;
	const BOOL fRead = ::ReadFile(h, Raw.data(), Size, &Read, nullptr);
	::CloseHandle(h);
	if (!fRead)
		return 0;

	//	UTF-8 で読む (BOM があれば飛ばす)
	const char *pBody = Raw.data();
	if (Read >= 3 && static_cast<BYTE>(pBody[0]) == 0xEF
			&& static_cast<BYTE>(pBody[1]) == 0xBB
			&& static_cast<BYTE>(pBody[2]) == 0xBF)
		pBody += 3;

	const int Len = ::MultiByteToWideChar(CP_UTF8, 0, pBody, -1, nullptr, 0);
	if (Len <= 0)
		return 0;
	std::wstring Text(Len, L'\0');
	::MultiByteToWideChar(CP_UTF8, 0, pBody, -1, &Text[0], Len);

	int Count = 0;
	size_t p = 0;
	while (p < Text.size()) {
		size_t q = Text.find_first_of(L"\r\n", p);
		if (q == std::wstring::npos)
			q = Text.size();
		std::wstring Line = Text.substr(p, q - p);
		p = q + 1;

		//	`#` から後ろは覚え書き
		const size_t Hash = Line.find(L'#');
		if (Hash != std::wstring::npos)
			Line.erase(Hash);

		const size_t Eq = Line.find(L'=');
		if (Eq == std::wstring::npos)
			continue;

		std::wstring Key = Line.substr(0, Eq);
		std::wstring Val = Line.substr(Eq + 1);
		const WCHAR *pszTrim = L" \t\r\n\0";
		Key.erase(0, Key.find_first_not_of(pszTrim));
		Key.erase(Key.find_last_not_of(pszTrim) + 1);
		Val.erase(0, Val.find_first_not_of(pszTrim));
		Val.erase(Val.find_last_not_of(pszTrim) + 1);

		std::string Bin;
		if (Val.empty() || !FromHex(Key, &Bin))
			continue;
		g_UserMap[Bin] = Val;
		Count++;
	}

	if (Count > 0) {
		WCHAR sz[MAX_PATH + 96];
		::StringCchPrintfW(sz, ARRAYSIZE(sz),
						   L"TSMemory: 外字の対応表を %d 件読み込みました : %s",
						   Count, szPath);
		TSMemoryLog(sz);
	}
	return Count;
}


bool TSMemoryDrcsReplaceFindByMd5(const BYTE *pMd5_16, std::wstring *pOut)
{
	pOut->clear();

	//	**利用者の分を先に見る。**局によって字形が違う物や、
	//	組み込みの表が合わない物を上書きできるようにする
	auto it = g_UserMap.find(std::string(reinterpret_cast<const char *>(pMd5_16), 16));
	if (it != g_UserMap.end()) {
		*pOut = it->second;
		return true;
	}

	//	組み込みの表は md5 の昇順に並んでいる
	int Lo = 0, Hi = TSMemoryDrcsMapCount - 1;
	while (Lo <= Hi) {
		const int Mid = (Lo + Hi) / 2;
		const int Cmp = ::memcmp(pMd5_16, TSMemoryDrcsMap[Mid].Md5, 16);
		if (Cmp == 0) {
			*pOut = FromCodePoint(TSMemoryDrcsMap[Mid].Code);
			return !pOut->empty();
		}
		if (Cmp < 0)
			Hi = Mid - 1;
		else
			Lo = Mid + 1;
	}
	return false;
}


bool TSMemoryDrcsReplaceFind(const TSMemoryDrcsGlyph &Glyph,
							 std::wstring *pOut, std::wstring *pMd5)
{
	pOut->clear();
	if (pMd5 != nullptr)
		pMd5->clear();
	if (Glyph.Pattern.empty())
		return false;

	BYTE Digest[16];
	if (!Md5Of(Glyph.Pattern, Digest))
		return false;
	if (pMd5 != nullptr)
		*pMd5 = ToHex(Digest);

	return TSMemoryDrcsReplaceFindByMd5(Digest, pOut);
}
