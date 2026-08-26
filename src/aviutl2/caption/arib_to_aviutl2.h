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

	//	放送の背景色を影・縁色として出すか。
	//
	//	**AviUtl2 のテキストには「背景の箱」が無い。**
	//	制御文字で指定できる 2 つ目の色は影・縁色 (aviutl2.txt の
	//	「色の変更(文字色,影縁色)」) で、放送の黒い箱そのものは作れない。
	//	縁取りで代用する形になる為、実際に見えるようにするには
	//	Preset のテキストプリセット側で文字装飾を縁取りにする必要がある。
	bool UseBroadcastBackColor = true;

	//	放送の文字の大きさに合わせるか。
	//
	//	字幕は「字幕平面の中で何ドット角か」で指定されて来る
	//	(例: 960x540 の平面で 36 ドット角)。ScreenHeight と合わせて
	//	AviUtl2 のサイズに直す。1080p なら 36 * 1080 / 540 = 72。
	//	0 にするとテキストプリセット側のサイズのままになる。
	bool UseBroadcastSize = false;

	//	出力の高さ (ピクセル)。UseBroadcastSize の時に要る
	int ScreenHeight = 0;

	//	外字に割り当てる私用領域の先頭
	wchar_t DrcsFirstCode = 0xE000;
};

//	色番号 (0-127) の RGB が判っているか。
//
//	**判っているのは既定の色配列 (CLUT 0) の 16 色だけ。**
//	COL の色配列選択で 1 以上が選ばれた場合の 112 色は
//	ARIB STD-B24 の拡張パレットで、まだ実装していない。
//	判らない色は指定を出さず、テキストプリセット側の色に任せる。
bool AribColorIsKnown(int Index);

//	変換する。
//	pDrcsCodes には、割り当てた順に ARIB 側の外字符号が入る
//	(添字 i が DrcsFirstCode + i に対応する)。フォントを組み立てる側は
//	この並びと同じ順で字形を並べる。
std::wstring AribItemsToAviUtl2(const std::vector<AribItem> &Items,
								const AribToAviUtl2Options &Options,
								std::vector<int> *pDrcsCodes);

//	ARIB の色番号 (0-15) を RGB に直す。既定の CLUT
DWORD AribColorToRgb(int Index);
