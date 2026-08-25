//----------------------------------------------------------------------------
//	AviUtl2 のフィルタプリセットを配置したオブジェクトに適用する
//----------------------------------------------------------------------------
#include <windows.h>
#include <shlwapi.h>

#include <map>
#include <string>
#include <vector>

#include "plugin2.h"

#include "preset.h"

namespace {

std::wstring ToWide(const std::string &utf8)
{
	if (utf8.empty())
		return std::wstring();

	const int Length = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
											 static_cast<int>(utf8.size()), nullptr, 0);
	if (Length <= 0)
		return std::wstring();

	std::wstring Out(static_cast<size_t>(Length), L'\0');
	::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
						  &Out[0], Length);
	return Out;
}

bool ReadFileUtf8(LPCWSTR path, std::string *out)
{
	HANDLE hFile = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
								 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
		return false;

	LARGE_INTEGER Size = {};
	//	プリセットは大きくても数十 KB 程度。異常に大きい物は読まない。
	if (!::GetFileSizeEx(hFile, &Size) || Size.QuadPart <= 0
			|| Size.QuadPart > 4 * 1024 * 1024) {
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

	//	BOM が付いていても読めるようにする
	if (Buffer.size() >= 3 && static_cast<unsigned char>(Buffer[0]) == 0xEF
			&& static_cast<unsigned char>(Buffer[1]) == 0xBB
			&& static_cast<unsigned char>(Buffer[2]) == 0xBF) {
		Buffer.erase(0, 3);
	}

	*out = Buffer;
	return true;
}

//	AviUtl2 のデータフォルダの候補を並べる。
//	通常は %ProgramData%\aviutl2 だが、プラグインは
//	  <データフォルダ>\Plugin\... に置かれる為、自分の位置から辿れる。
void CollectDataDirs(HMODULE self, std::vector<std::wstring> *out)
{
	if (self != nullptr) {
		WCHAR szPath[MAX_PATH] = {};
		if (::GetModuleFileNameW(self, szPath, MAX_PATH) != 0) {
			::PathRemoveFileSpecW(szPath);
			//	Plugin\TSMemory\ のように 1 段深い場合もあるので数段さかのぼる
			for (int i = 0; i < 4; i++) {
				out->push_back(szPath);
				if (!::PathRemoveFileSpecW(szPath))
					break;
			}
		}
	}

	WCHAR szProgramData[MAX_PATH] = {};
	if (::GetEnvironmentVariableW(L"ProgramData", szProgramData, MAX_PATH) != 0) {
		::PathAppendW(szProgramData, L"aviutl2");
		out->push_back(szProgramData);
	}
}

//	Preset フォルダから <種別>.<プリセット名>.preset を探す
bool FindPresetFile(LPCWSTR name, HMODULE self, std::wstring *out)
{
	std::vector<std::wstring> Dirs;
	CollectDataDirs(self, &Dirs);

	for (const std::wstring &Dir : Dirs) {
		WCHAR szPattern[MAX_PATH] = {};
		::lstrcpynW(szPattern, Dir.c_str(), MAX_PATH);
		::PathAppendW(szPattern, L"Preset");
		if (!::PathIsDirectoryW(szPattern))
			continue;

		std::wstring PresetDir = szPattern;

		//	AviUtl2 は「動画ファイル.キャプチャ用フィルタ.preset」の様に
		//	対象のオブジェクト種別を前置きする。種別は問わずに探す。
		::PathAppendW(szPattern, L"*.preset");

		WIN32_FIND_DATAW fd = {};
		HANDLE hFind = ::FindFirstFileW(szPattern, &fd);
		if (hFind == INVALID_HANDLE_VALUE)
			continue;

		std::wstring Found;
		do {
			if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
				continue;

			//	<種別>.<名前>.preset の <名前> の部分を取り出す
			std::wstring FileName = fd.cFileName;
			const size_t ExtPos = FileName.rfind(L".preset");
			if (ExtPos == std::wstring::npos)
				continue;
			std::wstring Stem = FileName.substr(0, ExtPos);

			const size_t DotPos = Stem.rfind(L'.');
			const std::wstring PresetName =
				(DotPos == std::wstring::npos) ? Stem : Stem.substr(DotPos + 1);

			if (::lstrcmpiW(PresetName.c_str(), name) != 0)
				continue;

			WCHAR szFull[MAX_PATH] = {};
			::lstrcpynW(szFull, PresetDir.c_str(), MAX_PATH);
			::PathAppendW(szFull, fd.cFileName);

			//	動画ファイル向けの物を優先する (他の種別でも一応使える)
			const bool fForVideoFile =
				(DotPos != std::wstring::npos) && (Stem.compare(0, DotPos, L"動画ファイル") == 0);
			if (Found.empty() || fForVideoFile)
				Found = szFull;
		} while (::FindNextFileW(hFind, &fd));
		::FindClose(hFind);

		if (!Found.empty()) {
			*out = Found;
			return true;
		}
	}
	return false;
}

//	プリセットの 1 エフェクト分
struct PresetEffect {
	std::wstring Name;
	bool Disabled = false;
	std::vector<std::pair<std::wstring, std::string>> Items;
};

std::string Trim(const std::string &s)
{
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
		b++;
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
		e--;
	return s.substr(b, e - b);
}

void Parse(const std::string &data, std::vector<PresetEffect> *out)
{
	bool fInEffect = false;

	size_t Pos = 0;
	while (Pos <= data.size()) {
		size_t End = data.find('\n', Pos);
		if (End == std::string::npos)
			End = data.size();
		const std::string Line = Trim(data.substr(Pos, End - Pos));
		Pos = End + 1;

		if (Line.empty() || Line[0] == ';')
			continue;

		if (Line[0] == '[') {
			//	[Effect.N] だけを対象にする ([Preset] などは読み飛ばす)
			fInEffect = (Line.compare(0, 8, "[Effect.") == 0);
			if (fInEffect)
				out->push_back(PresetEffect());
			continue;
		}

		if (!fInEffect || out->empty())
			continue;

		const size_t Eq = Line.find('=');
		if (Eq == std::string::npos)
			continue;

		const std::string Key = Trim(Line.substr(0, Eq));
		const std::string Value = Line.substr(Eq + 1);
		PresetEffect &Effect = out->back();

		if (Key == "effect.name") {
			Effect.Name = ToWide(Value);
		} else if (Key == "effect.disable") {
			Effect.Disabled = (Value != "0");
		} else if (!Key.empty()) {
			Effect.Items.push_back(std::make_pair(ToWide(Key), Value));
		}
	}

	//	名前の無いエフェクトは扱えない
	for (size_t i = out->size(); i > 0; i--) {
		if ((*out)[i - 1].Name.empty())
			out->erase(out->begin() + (i - 1));
	}
}

}	// namespace

bool TSMemoryPresetLoad(LPCWSTR name, LPCWSTR file, HMODULE self, TSMemoryPreset *out)
{
	out->Path.clear();
	out->Data.clear();

	std::wstring Path;
	if (file != nullptr && file[0] != L'\0') {
		Path = file;
		if (!::PathFileExistsW(Path.c_str()))
			return false;
	} else if (name == nullptr || name[0] == L'\0') {
		return false;
	} else if (!FindPresetFile(name, self, &Path)) {
		return false;
	}

	if (!ReadFileUtf8(Path.c_str(), &out->Data))
		return false;

	out->Path = Path;
	return true;
}

void TSMemoryPresetApply(EDIT_SECTION *edit, void *object, const TSMemoryPreset &preset,
						 TSMemoryPresetResult *out)
{
	if (edit == nullptr || object == nullptr || preset.IsEmpty())
		return;

	std::vector<PresetEffect> Effects;
	Parse(preset.Data, &Effects);

	//	同名のエフェクトが複数ある場合は find_effect() の ":n" で指定する
	std::map<std::wstring, int> Seen;

	for (const PresetEffect &Effect : Effects) {
		const int Index = Seen[Effect.Name]++;

		WCHAR szKey[128];
		if (Index == 0)
			::lstrcpynW(szKey, Effect.Name.c_str(), 128);
		else
			::wnsprintfW(szKey, 128, L"%s:%d", Effect.Name.c_str(), Index);

		//	既にあるエフェクト (動画ファイル・映像再生など) は作り直さず設定だけ変える
		EFFECT_HANDLE Handle = edit->find_effect(object, szKey);
		const bool fExisting = (Handle != nullptr);
		if (Handle == nullptr)
			Handle = edit->create_effect(object, Effect.Name.c_str());

		if (Handle == nullptr) {
			out->Failed++;
			if (out->FirstFailure.empty())
				out->FirstFailure = Effect.Name;
			continue;
		}
		out->Effects++;

		for (const auto &Item : Effect.Items) {
			//	取り込んだ .tvtv のパスをプリセットの物で上書きさせない。
			//	プリセットは作成時に開いていたファイルのパスを持っている。
			if (fExisting && Item.first == L"ファイル")
				continue;

			if (edit->set_effect_item_value(Handle, Item.first.c_str(), Item.second.c_str())) {
				out->Items++;
			} else {
				out->Failed++;
				if (out->FirstFailure.empty())
					out->FirstFailure = Effect.Name + L" / " + Item.first;
			}
		}

		if (Effect.Disabled)
			edit->set_effect_enable(Handle, false);
	}
}
