//----------------------------------------------------------------------------
//	ARIB STD-B24 の 8 単位符号を解く
//
//	字幕の本文は独自の符号系で書かれている。1 バイトの領域 (GL/GR) に
//	どの文字集合を割り当てるかを、エスケープと単/複シフトで切り替えながら
//	進む。制御符号で色・大きさ・位置も指定される。
//
//	ここでは「読める形」に直すところまでを担う。
//	AviUtl2 のテキスト制御文字への変換は呼び出し側で行う
//	(<#色> <s大きさ> <p座標> 等に写す)。
//
//	漢字は JIS X 0208 なので Shift_JIS に直して Windows に変換させる。
//	14KB の対応表を抱えずに済む。
//----------------------------------------------------------------------------
#pragma once

#include <windows.h>

#include <string>
#include <vector>

//	復号した結果の 1 項目
enum class AribItemType {
	Text,			// 本文
	Drcs,			// 外字。A = 符号 (どの字形かは別途 DRCS 定義から引く)
	Color,			// A = 前景色 (0-15)
	Size,			// A = 0:標準 1:中型 2:小型 3:倍角
	Position,		// A = 桁, B = 行 (APS)
	ClearScreen,	// 画面消去 (CS)
	LineBreak,		// 改行 (APD / APR)
};

struct AribItem {
	AribItemType Type;
	std::wstring Text;		// Text の時だけ
	int A = 0;
	int B = 0;
};

//	字幕文データ (data_unit の本文) を解く。
//	pData/Size … データユニットの中身 (0x20 = 本文)
//	pOut       … 解けた項目が順に入る
void AribDecodeText(const BYTE *pData, size_t Size, std::vector<AribItem> *pOut);

//	項目の並びから本文だけを取り出す (確認用)。
//	外字は pszDrcs (既定は "＃") に置き換える
std::wstring AribItemsToPlainText(const std::vector<AribItem> &Items,
								  LPCWSTR pszDrcs = L"＃");
