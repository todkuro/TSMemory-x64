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

	//	**前回までの取り込みで受け取った字形を使い回すか。**
	//	(`CTSCaptionSource` だけが見る。文字列への変換には関係しない)
	//
	//	字形は数十秒おきにしか流れて来ないので、既定の `MemorySize` では
	//	半々くらいで窓に入らず `《` `》` 等が代替文字になる。
	//	**符号 (0x21 から順) の意味は番組ごとに変わる**ので、
	//	同じ字幕 PID の間だけ、一定時間だけ覚えておく
	bool UseGlyphCache = true;

	//	放送の色をそのまま使うか。
	//	0 にするとプリセット側の色に任せる (<#...> を出さない)
	bool UseBroadcastColor = true;

	//	ルビを `</>漢字<!>ふりがな</>` にするか。
	//
	//	**ルビは小型 (SSZ) で本文の 1 行上に、まとまりごとに位置を
	//	打って書かれる。**そのままだと本文の頭にふりがなだけが
	//	並んでしまう (実測: `<s><s*0.5>あいぞううずま<s>♬ 愛憎愛憎渦巻いて`)。
	//	X の重なりで本文のどの字に掛かるかを決めて括る。
	//
	//	0 にすると従来どおり小型のまま本文に混ぜる。aviutl2.txt に
	//	「制御文字との組み合わせによっては正しく描画出来ない場合が
	//	あります」とある為、逃げ道を残してある
	bool UseRuby = true;

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

//	字幕の 1 行。
//
//	**放送は行ごとに座標を持っている。**まとめて 1 つのオブジェクトに
//	すると、行の長さが違っても背景が全体を囲む 1 つの箱になってしまう。
//	行ごとに置けば位置も背景も放送どおりになる。
struct AribCaptionLine {
	std::wstring Text;		// AviUtl2 のテキスト (改行は含まない)
	int Left = -1;			// 字幕平面の中での左上 (ドット)
	int Top = -1;
	//	行の右端 (ドット)。**同じ高さで横に飛んだ時に行を割る**のに要る。
	//	複数の話者を同時に別の場所へ出す字幕で、書き終えた所より
	//	送り幅より離れた位置に書き直されたら別の行として扱う
	int Right = -1;
};

//	字幕が画面のどこに出るか。
//
//	**字幕平面の中での位置**なので、出力の解像度に合わせて割り直す。
//	ACPS が指しているのは行の**下端**なので、1 行分の高さを引いて
//	上端に直してある。
struct AribCaptionLayout {
	std::vector<AribCaptionLine> Lines;
	int PlaneWidth = 960;
	int PlaneHeight = 540;

	bool IsValid() const
	{
		return !Lines.empty() && PlaneWidth > 0 && PlaneHeight > 0;
	}
};

//	色番号 (0-127) に色が付いているか。
//	透明 (アルファ 0) なら false。背景の既定は 8 番 = 透明
bool AribColorIsKnown(int Index);

//	色番号の不透明度 (0-255)。背景の既定 (8 番) は 0 = 透明
int AribColorAlpha(int Index);

//	変換する。
//	pDrcsCodes には、割り当てた順に ARIB 側の外字符号が入る
//	(添字 i が DrcsFirstCode + i に対応する)。フォントを組み立てる側は
//	この並びと同じ順で字形を並べる。
//	pLayout に画面上の位置が入る (要らなければ nullptr)。
std::wstring AribItemsToAviUtl2(const std::vector<AribItem> &Items,
								const AribToAviUtl2Options &Options,
								std::vector<int> *pDrcsCodes,
								AribCaptionLayout *pLayout = nullptr);

//	ARIB の色番号 (0-15) を RGB に直す。既定の CLUT
DWORD AribColorToRgb(int Index);
