//----------------------------------------------------------------------------
//	*.tvtv の字幕
//
//	bridge.cpp から見える唯一の窓口。字幕対応は
//	src/aviutl2/caption/ に閉じており、このヘッダだけを include すれば足りる。
//	無効時 (`[Caption] Enable=0`) はこのクラスを作らないので、
//	字幕側のコードは一切動かない。
//
//	中でやっている事:
//	  ts_caption     … 共有メモリの TS から字幕のデータユニットを取り出す
//	  arib_text      … ARIB STD-B24 の 8 単位符号を解く
//	  arib_to_aviutl2… AviUtl2 のテキスト制御文字に直す
//	  drcs_ttf       … 外字の字形を TrueType に組み立てる
//	  drcs_font      … それを AviUtl2 に登録する
//----------------------------------------------------------------------------
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "arib_to_aviutl2.h"
#include "drcs_ttf.h"

//	1 つの字幕
struct TSMemoryCaption {
	//	映像の先頭からの秒数。負なら不明
	double Seconds = -1.0;

	//	AviUtl2 のテキスト (制御文字を含む)。**1 行分**。
	//	放送は行ごとに座標を持っているので、行ごとに 1 件にする
	std::wstring Text;

	//	字幕平面の中での位置 (ドット)。負なら判らなかった
	int Left = -1;
	int Top = -1;
	int PlaneWidth = 960;
	int PlaneHeight = 540;

	bool HasPosition() const
	{
		return Left >= 0 && Top >= 0 && PlaneWidth > 0 && PlaneHeight > 0;
	}
};

class CTSCaptionSource
{
public:
	CTSCaptionSource();
	~CTSCaptionSource();

	CTSCaptionSource(const CTSCaptionSource &) = delete;
	CTSCaptionSource &operator=(const CTSCaptionSource &) = delete;

	//	共有メモリ名 (= .tvtv のファイル名部分) を渡して開く
	bool Open(const char *pszSharedName, const AribToAviUtl2Options &Options);

	int GetCount() const { return static_cast<int>(m_Captions.size()); }
	const TSMemoryCaption &Get(int Index) const { return m_Captions[Index]; }

	//	組み立てた外字フォント (空なら外字は無かった)
	const std::vector<BYTE> &GetFont() const { return m_Font; }
	int GetGlyphCount() const { return static_cast<int>(m_GlyphCount); }

	//	字形が届かなかった外字の数 (リングバッファの窓の外で定義された物)
	int GetMissingGlyphCount() const { return m_MissingGlyphs; }

	LPCWSTR GetLastError() const { return m_szError; }

private:
	std::vector<TSMemoryCaption> m_Captions;
	std::vector<BYTE> m_Font;
	size_t m_GlyphCount = 0;
	int m_MissingGlyphs = 0;
	WCHAR m_szError[128] = {};
};
