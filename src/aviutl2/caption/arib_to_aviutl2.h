//----------------------------------------------------------------------------
//	復号した字幕を AviUtl2 のテキストに直す
//
//	AviUtl2 のテキストオブジェクトは制御文字を解釈する。ARIB 字幕の
//	表現とほぼ一対一で対応する為、焼き込まずにテキストのまま出せる。
//
//	  文字色     <#ffffff>
//	  大きさ     <s*0.5>
//	  プリセット <$名前>      ← **書体を一括で変える為の要**
//	  フォント   <@名前>      ← 外字だけ切り替える
//
//	`<$字幕>` を先頭に置くと、利用者が AviUtl2 側でその
//	テキストプリセットを 1 つ直すだけで全ての字幕に効く。
//	タイムライン上で個別に触る必要が無い。
//----------------------------------------------------------------------------
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "arib_text.h"

struct AribToAviUtl2Options {
	//	本文の先頭に入れる <$...> の名前。空なら入れない
	std::wstring Preset;

	//	外字用フォント名 (drcs_ttf.h で組み立てた物)。
	//	空なら外字は Fallback の文字に置き換える
	std::wstring DrcsFont;

	//	外字を表せない時の文字
	std::wstring DrcsFallback = L"〓";

	//	放送の色をそのまま使うか。
	//	0 にするとプリセット側の色に任せる (<#...> を出さない)
	bool UseBroadcastColor = true;

	//	外字に割り当てる私用領域の先頭
	wchar_t DrcsFirstCode = 0xE000;
};

//	変換する。
//	pDrcsCodes には、割り当てた順に ARIB 側の外字符号が入る
//	(添字 i が DrcsFirstCode + i に対応する)。フォントを組み立てる側は
//	この並びと同じ順で字形を並べる。
std::wstring AribItemsToAviUtl2(const std::vector<AribItem> &Items,
								const AribToAviUtl2Options &Options,
								std::vector<int> *pDrcsCodes);

//	ARIB の色番号 (0-15) を RGB に直す。既定の CLUT
DWORD AribColorToRgb(int Index);
