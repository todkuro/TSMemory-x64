#pragma once

#include <string>

struct EDIT_SECTION;

//	フィルタプリセットを配置したオブジェクトに適用する。
//
//	AviUtl2 の「フィルタのプリセット」は
//	  <AviUtl2 のデータフォルダ>\Preset\<オブジェクト種別>.<プリセット名>.preset
//	という UTF-8 のテキストファイルで、エイリアスファイルと同じ
//	[Effect.N] / キー=値 の形式になっている。
//
//	これを読んで EDIT_SECTION の create_effect() / set_effect_item_value() で
//	組み立て直す事で、取り込んだ映像に毎回同じフィルタ構成を掛けられる。

struct TSMemoryPreset {
	std::wstring Path;		//	読み込んだファイルのパス (診断用)
	std::string Data;		//	ファイルの中身 (UTF-8)

	bool IsEmpty() const { return Data.empty(); }
};

struct TSMemoryPresetResult {
	int Effects = 0;		//	作成・設定したエフェクトの数
	int Items = 0;			//	設定出来た項目の数
	int Failed = 0;			//	設定出来なかった項目の数
	std::wstring FirstFailure;	//	最初に失敗した項目 (診断用)
};

//	プリセットを探して読み込む。
//	name : プリセット名 (空なら何もしない)
//	file : プリセットファイルの明示指定 (空なら name から探す)
//	戻り値: 見つかって読み込めた場合は true
bool TSMemoryPresetLoad(LPCWSTR name, LPCWSTR file, HMODULE self, TSMemoryPreset *out);

//	読み込んだプリセットをオブジェクトに適用する
void TSMemoryPresetApply(EDIT_SECTION *edit, void *object, const TSMemoryPreset &preset,
						 TSMemoryPresetResult *out);
