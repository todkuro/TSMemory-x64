//----------------------------------------------------------------------------
//	UTF-8 で書かれた設定ファイルから文字列を読む
//----------------------------------------------------------------------------
#include <windows.h>

#include <string>

#include "inifile.h"

namespace {

//	設定ファイルは大きくても数 KB。異常に大きい物は相手にしない。
const DWORD MAX_INI_SIZE = 1024 * 1024;

bool ReadWholeFile(LPCWSTR path, std::string *out)
{
	HANDLE hFile = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
								 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
		return false;

	LARGE_INTEGER Size = {};
	if (!::GetFileSizeEx(hFile, &Size) || Size.QuadPart <= 0 || Size.QuadPart > MAX_INI_SIZE) {
		::CloseHandle(hFile);
		return false;
	}

	std::string Buffer(static_cast<size_t>(Size.QuadPart), '\0');
	DWORD Read = 0;
	const BOOL fOK = ::ReadFile(hFile, &Buffer[0], static_cast<DWORD>(Buffer.size()),
								&Read, nullptr);
	::CloseHandle(hFile);
	if (!fOK)
		return false;

	Buffer.resize(Read);
	*out = Buffer;
	return true;
}

std::string Trim(const std::string &s)
{
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
		b++;
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
		e--;
	return s.substr(b, e - b);
}

bool EqualsNoCaseAscii(const std::string &a, LPCWSTR b)
{
	//	節・キーの名前は ASCII 前提 (AviUtl2 の設定も全て ASCII)
	size_t i = 0;
	for (; i < a.size() && b[i] != L'\0'; i++) {
		wchar_t x = static_cast<unsigned char>(a[i]), y = b[i];
		if (x >= L'A' && x <= L'Z') x += L'a' - L'A';
		if (y >= L'A' && y <= L'Z') y += L'a' - L'A';
		if (x != y)
			return false;
	}
	return i == a.size() && b[i] == L'\0';
}

//	UTF-8 として解釈出来るファイルからキーの値を探す
bool FindValueUtf8(const std::string &data, LPCWSTR section, LPCWSTR key, std::string *out)
{
	bool fInSection = false;

	size_t Pos = 0;
	while (Pos <= data.size()) {
		size_t End = data.find('\n', Pos);
		if (End == std::string::npos)
			End = data.size();
		const std::string Line = Trim(data.substr(Pos, End - Pos));
		Pos = End + 1;

		if (Line.empty() || Line[0] == ';' || Line[0] == '#')
			continue;

		if (Line[0] == '[') {
			const size_t Close = Line.find(']');
			if (Close == std::string::npos)
				continue;
			fInSection = EqualsNoCaseAscii(Trim(Line.substr(1, Close - 1)), section);
			continue;
		}

		if (!fInSection)
			continue;

		const size_t Eq = Line.find('=');
		if (Eq == std::string::npos)
			continue;
		if (!EqualsNoCaseAscii(Trim(Line.substr(0, Eq)), key))
			continue;

		std::string Value = Trim(Line.substr(Eq + 1));

		//	GetPrivateProfileString() に合わせて前後の " を外す
		if (Value.size() >= 2 && Value.front() == '"' && Value.back() == '"')
			Value = Value.substr(1, Value.size() - 2);

		*out = Value;
		return true;
	}
	return false;
}

}	// namespace

DWORD TSMemoryGetIniString(LPCWSTR ini, LPCWSTR section, LPCWSTR key, LPCWSTR def,
						   LPWSTR out, DWORD size)
{
	if (out == nullptr || size == 0)
		return 0;
	out[0] = L'\0';

	if (ini != nullptr && ini[0] != L'\0') {
		std::string Data;
		if (ReadWholeFile(ini, &Data)) {
			const bool fUtf16 = Data.size() >= 2
				&& ((static_cast<unsigned char>(Data[0]) == 0xFF
					 && static_cast<unsigned char>(Data[1]) == 0xFE)
					|| (static_cast<unsigned char>(Data[0]) == 0xFE
						&& static_cast<unsigned char>(Data[1]) == 0xFF));

			if (!fUtf16) {
				if (Data.size() >= 3 && static_cast<unsigned char>(Data[0]) == 0xEF
						&& static_cast<unsigned char>(Data[1]) == 0xBB
						&& static_cast<unsigned char>(Data[2]) == 0xBF) {
					Data.erase(0, 3);
				}

				//	UTF-8 として妥当な場合のみ自前で読む。
				//	CP932 のファイルはここで弾かれ、Win32 側に回る。
				const bool fUtf8 = Data.empty()
					|| ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Data.c_str(),
											 static_cast<int>(Data.size()), nullptr, 0) > 0;
				if (fUtf8) {
					std::string Value;
					if (!FindValueUtf8(Data, section, key, &Value)) {
						::lstrcpynW(out, def != nullptr ? def : L"", static_cast<int>(size));
						return static_cast<DWORD>(::lstrlenW(out));
					}
					//	一旦フルの長さに変換してから写す。
					//	out へ直接変換すると、収まらない時に
					//	MultiByteToWideChar() が 0 を返して**値が丸ごと
					//	空になる**。GetPrivateProfileString() は切り詰める
					//	ので、そちらに合わせる。
					std::wstring Wide;
					const int Need = ::MultiByteToWideChar(
						CP_UTF8, 0, Value.c_str(), static_cast<int>(Value.size()),
						nullptr, 0);
					if (Need > 0) {
						Wide.resize(static_cast<size_t>(Need));
						::MultiByteToWideChar(CP_UTF8, 0, Value.c_str(),
											  static_cast<int>(Value.size()), &Wide[0], Need);
					}
					::lstrcpynW(out, Wide.c_str(), static_cast<int>(size));
					return static_cast<DWORD>(::lstrlenW(out));
				}
			}
		}
	}

	return ::GetPrivateProfileStringW(section, key, def, out, size, ini);
}
