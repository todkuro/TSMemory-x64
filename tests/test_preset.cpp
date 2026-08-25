//----------------------------------------------------------------------------
//	フィルタプリセットの読み込みと適用を確認する。
//
//	  test_preset [work-dir]
//
//	AviUtl2 の EDIT_SECTION のふりをして、プリセットの通りに
//	エフェクトが組み立てられるかを見る。src/aviutl2/preset.cpp を直接
//	テストに取り込んでいる (aux2 からは公開していない為)。
//----------------------------------------------------------------------------
#include <windows.h>
#include <shlwapi.h>
#include <cstdio>
#include <string>
#include <vector>

#include "plugin2.h"
#include "preset.h"

namespace {

int g_failures = 0;

void check(const char *what, bool ok)
{
	std::printf("%-56s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		g_failures++;
}

//---------------------------------------------------------------------------
//	AviUtl2 のオブジェクト / エフェクトのふり
//---------------------------------------------------------------------------
struct FakeEffect {
	std::wstring Name;
	bool Enabled = true;
	std::vector<std::pair<std::wstring, std::string>> Items;

	const std::string *Find(LPCWSTR item) const
	{
		for (const auto &e : Items) {
			if (e.first == item)
				return &e.second;
		}
		return nullptr;
	}
};

struct FakeObject {
	std::vector<FakeEffect *> Effects;

	FakeEffect *Add(LPCWSTR name)
	{
		FakeEffect *p = new FakeEffect();
		p->Name = name;
		Effects.push_back(p);
		return p;
	}
};

FakeObject g_Object;

//	AviUtl2 と同じく "名前:n" のインデックス指定に対応する
EFFECT_HANDLE fake_find_effect(OBJECT_HANDLE object, LPCWSTR effect)
{
	if (object != &g_Object || effect == nullptr)
		return nullptr;

	std::wstring Key = effect;
	int Index = 0;
	const size_t Colon = Key.rfind(L':');
	if (Colon != std::wstring::npos) {
		Index = ::_wtoi(Key.c_str() + Colon + 1);
		Key = Key.substr(0, Colon);
	}

	int Seen = 0;
	for (FakeEffect *p : g_Object.Effects) {
		if (p->Name == Key && Seen++ == Index)
			return p;
	}
	return nullptr;
}

EFFECT_HANDLE fake_create_effect(OBJECT_HANDLE object, LPCWSTR effect)
{
	if (object != &g_Object || effect == nullptr)
		return nullptr;
	return g_Object.Add(effect);
}

bool fake_set_effect_item_value(EFFECT_HANDLE effect, LPCWSTR item, LPCSTR value)
{
	FakeEffect *p = static_cast<FakeEffect *>(effect);
	if (p == nullptr || item == nullptr || value == nullptr)
		return false;

	//	AviUtl2 は存在しない項目には設定出来ない。失敗の数え方を確認する為に
	//	1 つだけ受け付けない項目を作っておく。
	if (::lstrcmpW(item, L"存在しない項目") == 0)
		return false;

	for (auto &e : p->Items) {
		if (e.first == item) {
			e.second = value;
			return true;
		}
	}
	p->Items.push_back(std::make_pair(std::wstring(item), std::string(value)));
	return true;
}

void fake_set_effect_enable(EFFECT_HANDLE effect, bool enable)
{
	FakeEffect *p = static_cast<FakeEffect *>(effect);
	if (p != nullptr)
		p->Enabled = enable;
}

//---------------------------------------------------------------------------
//	テスト用のプリセット (実際に AviUtl2 が書き出す物と同じ形)
//---------------------------------------------------------------------------
const char PRESET_TEXT[] =
	"[Preset]\r\n"
	"target=all\r\n"
	"[Effect.0]\r\n"
	"effect.name=\xE5\x8B\x95\xE7\x94\xBB\xE3\x83\x95\xE3\x82\xA1\xE3\x82\xA4\xE3\x83\xAB\r\n"	// 動画ファイル
	"\xE5\x86\x8D\xE7\x94\x9F\xE9\x80\x9F\xE5\xBA\xA6=100.00\r\n"								// 再生速度
	"\xE3\x83\x95\xE3\x82\xA1\xE3\x82\xA4\xE3\x83\xAB=C:\\old\\stale.tvtv\r\n"					// ファイル
	"[Effect.1]\r\n"
	"effect.name=\xE6\x98\xA0\xE5\x83\x8F\xE5\x86\x8D\xE7\x94\x9F\r\n"							// 映像再生
	"\xE5\x8F\x8D\xE8\xBB\xA2=0.00\r\n"															// 反転
	"[Effect.2]\r\n"
	"effect.name=\xE3\x82\xA4\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC\xE3\x83\xAC\xE3\x83\xBC"
	"\xE3\x82\xB9\xE8\xA7\xA3\xE9\x99\xA4\r\n"													// インターレース解除
	"effect.disable=1\r\n"
	"\xE8\xA7\xA3\xE9\x99\xA4\xE6\x96\xB9\xE6\xB3\x95=\xE4\xBA\x8C\xE9\x87\x8D\xE5\x8C\x96\r\n"	// 解除方法=二重化
	"[Effect.3]\r\n"
	"effect.name=\xE3\x83\x8E\xE3\x82\xA4\xE3\x82\xBA\xE9\x99\xA4\xE5\x8E\xBB\r\n"				// ノイズ除去
	"\xE5\xBC\xB7\xE5\xBA\xA6=60.0\r\n"															// 強度
	"\xE5\xAD\x98\xE5\x9C\xA8\xE3\x81\x97\xE3\x81\xAA\xE3\x81\x84\xE9\xA0\x85\xE7\x9B\xAE=1\r\n"	// 存在しない項目
	"[Effect.4]\r\n"
	"effect.name=\xE3\x82\xB7\xE3\x83\xA3\xE3\x83\xBC\xE3\x83\x97\r\n"							// シャープ
	"\xE5\xBC\xB7\xE5\xBA\xA6=15.0\r\n";

bool WriteAll(LPCWSTR path, const char *data, size_t size)
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

}	// namespace

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	//	--- プリセットを置く場所を用意する -----------------------------------
	WCHAR szWork[MAX_PATH] = {};
	if (argc > 1) {
		::MultiByteToWideChar(CP_ACP, 0, argv[1], -1, szWork, MAX_PATH);
	} else {
		::GetTempPathW(MAX_PATH, szWork);
		::PathAppendW(szWork, L"tsmemory_preset_test");
	}
	::CreateDirectoryW(szWork, nullptr);

	WCHAR szData[MAX_PATH] = {};
	::lstrcpynW(szData, szWork, MAX_PATH);
	::PathAppendW(szData, L"aviutl2");
	::CreateDirectoryW(szData, nullptr);

	WCHAR szPresetDir[MAX_PATH] = {};
	::lstrcpynW(szPresetDir, szData, MAX_PATH);
	::PathAppendW(szPresetDir, L"Preset");
	::CreateDirectoryW(szPresetDir, nullptr);

	WCHAR szPresetFile[MAX_PATH] = {};
	::lstrcpynW(szPresetFile, szPresetDir, MAX_PATH);
	::PathAppendW(szPresetFile, L"動画ファイル.キャプチャ用フィルタ.preset");

	check("wrote a preset file",
		  WriteAll(szPresetFile, PRESET_TEXT, sizeof(PRESET_TEXT) - 1));

	//	紛らわしい物を隣に置いて、名前で選び分けられる事を見る
	WCHAR szOther[MAX_PATH] = {};
	::lstrcpynW(szOther, szPresetDir, MAX_PATH);
	::PathAppendW(szOther, L"動画ファイル.別のプリセット.preset");
	WriteAll(szOther, "[Preset]\r\ntarget=all\r\n", 22);

	//	CollectDataDirs() が見に行く %ProgramData% を差し替える
	::SetEnvironmentVariableW(L"ProgramData", szWork);

	//	--- 名前で探せる事 ---------------------------------------------------
	TSMemoryPreset Preset;
	check("the preset was found by name",
		  TSMemoryPresetLoad(L"キャプチャ用フィルタ", L"", nullptr, &Preset));
	check("the right file was picked",
		  ::lstrcmpiW(Preset.Path.c_str(), szPresetFile) == 0);

	TSMemoryPreset Missing;
	check("an unknown preset name is reported as missing",
		  !TSMemoryPresetLoad(L"ないプリセット", L"", nullptr, &Missing));

	TSMemoryPreset ByPath;
	check("a preset can also be given by path",
		  TSMemoryPresetLoad(L"", szPresetFile, nullptr, &ByPath));

	//	--- 適用 --------------------------------------------------------------
	//	create_object_from_media_file() 直後の状態を作る
	FakeEffect *pMedia = g_Object.Add(L"動画ファイル");
	pMedia->Items.push_back(std::make_pair(std::wstring(L"ファイル"),
										   std::string("C:\\new\\tsmemory0_7.tvtv")));
	g_Object.Add(L"映像再生");

	EDIT_SECTION Edit = {};
	Edit.find_effect = fake_find_effect;
	Edit.create_effect = fake_create_effect;
	Edit.set_effect_item_value = fake_set_effect_item_value;
	Edit.set_effect_enable = fake_set_effect_enable;

	TSMemoryPresetResult Result;
	TSMemoryPresetApply(&Edit, &g_Object, Preset, &Result);

	std::printf("  effects=%d items=%d failed=%d\n",
				Result.Effects, Result.Items, Result.Failed);

	check("every effect in the preset was handled", Result.Effects == 5);

	//	既にあるエフェクトは作り直さない (動画ファイル / 映像再生 + 追加 3 つ)
	check("existing effects are reused, not duplicated", g_Object.Effects.size() == 5);

	const LPCWSTR Expected[] = {
		L"動画ファイル", L"映像再生", L"インターレース解除", L"ノイズ除去", L"シャープ",
	};
	bool fOrder = (g_Object.Effects.size() == 5);
	for (size_t i = 0; fOrder && i < 5; i++)
		fOrder = (g_Object.Effects[i]->Name == Expected[i]);
	check("the effects are in the order given by the preset", fOrder);

	//	取り込んだ .tvtv のパスがプリセットの古いパスで潰されない事
	const std::string *pFile = pMedia->Find(L"ファイル");
	check("the captured .tvtv path is not overwritten by the preset",
		  pFile != nullptr && *pFile == "C:\\new\\tsmemory0_7.tvtv");

	//	既存エフェクトのそれ以外の項目は反映される
	const std::string *pSpeed = pMedia->Find(L"再生速度");
	check("other items of an existing effect are applied",
		  pSpeed != nullptr && *pSpeed == "100.00");

	//	新しく作ったエフェクトの項目
	if (g_Object.Effects.size() == 5) {
		const std::string *pMethod = g_Object.Effects[2]->Find(L"解除方法");
		check("items of a newly created effect are applied",
			  pMethod != nullptr && *pMethod == "\xE4\xBA\x8C\xE9\x87\x8D\xE5\x8C\x96");

		//	effect.disable=1 のエフェクトは無効で追加される
		check("effect.disable=1 turns the effect off", !g_Object.Effects[2]->Enabled);
		check("the other effects stay enabled",
			  g_Object.Effects[3]->Enabled && g_Object.Effects[4]->Enabled);

		const std::string *pStrength = g_Object.Effects[4]->Find(L"強度");
		check("the last effect was applied too",
			  pStrength != nullptr && *pStrength == "15.0");
	}

	//	設定出来なかった項目は数えられている
	check("an item that cannot be set is counted as a failure", Result.Failed == 1);

	//	--- [Preset] セクションは読み飛ばされる事 -----------------------------
	//	target=all を項目として拾ってしまうと余計なエフェクトが出来る
	check("the [Preset] section is not turned into an effect", Result.Effects == 5);

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
				g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
