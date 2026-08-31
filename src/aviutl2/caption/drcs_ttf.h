//----------------------------------------------------------------------------
//	外字 (DRCS) のビットマップから TrueType フォントを組み立てる
//
//	ARIB 字幕の外字は、字形のビットマップ (実測では 36x36 / 4 階調) が
//	放送に乗って来る。これを輪郭に起こしてフォントに仕立てれば、
//	AviUtl2 の register_font_collection() に渡して**テキストのまま**
//	放送どおりの字形で表示出来る (drcs_font.h を参照)。
//
//	字形は 1 画素 = 1 矩形の輪郭にする。曲線近似はしない。
//	  ・元が 36x36 の点の集まりなので、なぞっても情報は増えない
//	  ・拡大時の見え方は DirectWrite のアンチエイリアスに任せる
//	  ・実装が単純で、出力を検証しやすい
//----------------------------------------------------------------------------
#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

struct TSMemoryDrcsGlyph {
	//	割り当てる Unicode。私用領域 (U+E000〜U+F8FF) を使う
	wchar_t Code;

	int Width;			// 画素数 (実測では 36)
	int Height;
	//	階調数 - 2 (0 = 2 階調 / 2 = 4 階調)。ARIB の depth と同じ
	int Depth;

	//	画素の並び。左上から右へ、1 画素あたり ceil(log2(Depth+2)) ビット。
	//	行の境目でバイト境界に揃えない (ARIB と同じ詰め方)
	std::vector<BYTE> Pattern;
};

//	フォントを組み立てる。
//
//	Glyphs   … 字形の並び。Code は重複しない事
//	pszFamily… フォント名 (ASCII 推奨。name テーブルに入る)
//	pOut     … 出来上がった TTF
//	戻り値   … 組み立てられたら true
bool TSMemoryBuildDrcsFont(const std::vector<TSMemoryDrcsGlyph> &Glyphs,
						   LPCWSTR pszFamily, std::vector<BYTE> *pOut);

//	ビットマップの画素を取り出す (0 〜 Depth+1)。
//	範囲外は 0。テストからも使う
int TSMemoryDrcsPixel(const TSMemoryDrcsGlyph &Glyph, int x, int y);
