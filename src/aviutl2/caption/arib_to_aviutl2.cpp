//----------------------------------------------------------------------------
//	復号した字幕を AviUtl2 のテキストに直す (arib_to_aviutl2.h を参照)
//----------------------------------------------------------------------------
#include <windows.h>

#include <algorithm>
#include <string>
#include <cwchar>
#include <vector>

#include "arib_to_aviutl2.h"
#include "arib_gaiji.h"

namespace {

//	制御文字と誤解される文字を避ける。
//
//	**AviUtl2 の仕様には '<' の打ち消し方が書かれていない。**
//	`aviutl2.txt` にも入力補助の `default.word` にも記述が無い。
//	その為 `<<` のような当て推量は使わず、**全角の '＜' に置き換える**。
//	字幕の本文はもともと全角なので、放送では ASCII の '<' はまず来ない
//	(英数集合も全角で出している)。来た場合も見た目は変わらない。
std::wstring Escape(const std::wstring &s)
{
	std::wstring Out;
	for (wchar_t c : s) {
		if (c == L'<')
			Out += L"＜";		// ＜
		else
			Out += c;
	}
	return Out;
}

//	10 倍で持っている倍率を "0.5" / "2" のような文字列にする
std::wstring ScaleName(int Tenth)
{
	if (Tenth % 10 == 0)
		return std::to_wstring(Tenth / 10);
	std::wstring s = std::to_wstring(Tenth / 10);
	s += L".";
	s += std::to_wstring(Tenth % 10);
	return s;
}

void AppendHex(std::wstring *pOut, DWORD Rgb)
{
	static const wchar_t Digits[] = L"0123456789abcdef";
	for (int i = 5; i >= 0; i--)
		*pOut += Digits[(Rgb >> (i * 4)) & 0x0F];
}

}	// namespace


bool AribColorIsKnown(int Index)
{
	//	**透明 (アルファ 0) は「色が無い」。**
	//	背景の既定は 8 番 = 透明なので、これを黒として出すと
	//	何も無い所に黒が付く
	return Index >= 0 && Index < 128 && ::TSMemoryAribClut(Index).A != 0;
}


int AribColorAlpha(int Index)
{
	return ::TSMemoryAribClut(Index).A;
}


DWORD AribColorToRgb(int Index)
{
	//	**ARIB の 128 色表そのまま。**
	//	以前は既定の 16 色だけを持っていた為、放送が使う色配列 4 の
	//	背景色 (索引 65 = 半透明の黒) を「判らない色」として捨てていた
	const TSMemoryAribColor c = ::TSMemoryAribClut(Index);
	return (static_cast<DWORD>(c.R) << 16)
		 | (static_cast<DWORD>(c.G) << 8)
		 |  static_cast<DWORD>(c.B);
}


std::wstring AribItemsToAviUtl2(const std::vector<AribItem> &Items,
								const AribToAviUtl2Options &Options,
								std::vector<int> *pDrcsCodes,
								AribCaptionLayout *pLayout)
{
	//	**位置は「本文が実際に書かれた所」だけを見る。**
	//	字幕平面の設定 (SDF/SDP) は本文の有無に関わらず送られて来るので、
	//	そちらを見ると何も書いていない字幕まで位置を持ってしまう
	int PlaneW = 960, PlaneH = 540;

	//	**行ごとに 1 つのオブジェクトにする。**
	//	放送は行ごとに座標を持っているので、まとめてしまうと
	//	位置も背景の箱も放送と合わなくなる
	std::vector<AribCaptionLine> Lines;
	AribCaptionLine Cur;

	//	**属性は「変わった時に、本文の直前で」出す。**
	//	受け取った順にそのまま出すと、放送側が本文の無い所で何度も
	//	指定し直す為 <#...><#...><s...> が延々と並び、読めなくなる。
	//	**放送の字幕は改行を送って来ない。**
	//	1 行ごとに ACPS で座標を打つ形なので、位置が下に動いたら
	//	そこが改行。読み飛ばすと全ての行が繋がって 1 行になる。
	//
	//	**ルビは行として数えない。**ルビは本文の 1 行上に、本文より
	//	先に書かれる (実測: ルビ y=449 → 本文 y=509、行送り 60)。
	//	位置が動いたら即改行にすると「ルビ / 本文」で毎回割れる。
	//	小型 (SSZ) で書かれているかどうかで見分ける
	int BaseY = -1;			// 直近の本文の行の Y
	int PendingY = -1;		// まだ本文が来ていない位置指定
	int PendingX = -1;
	int PendingPitch = 0;
	int PendingPitchX = 0;
	bool fAnyText = false;

	int Color = -1, EmittedColor = -1;
	int Back = -1, EmittedBack = -1;
	//	文字の大きさ。10 倍した整数で持つ (5 = 半分 / 20 = 倍)
	int ScaleH = 10, EmittedH = 10;
	int ScaleV = 10, EmittedV = 10;

	//	放送の文字の大きさ (AviUtl2 のサイズに直した値)。0 ならプリセット任せ
	int BaseSize = 0;
	bool fSizeEmitted = false;

	//	--- ルビ -------------------------------------------------------------
	//	**ルビは小型 (SSZ) で本文の 1 行上に書かれる。**
	//	まとまりごとに位置が打たれるので、X の重なりで本文のどの字に
	//	掛かるかが決まる (実測の生バイト):
	//
	//	  88                       SSZ (小型)
	//	  CSI "230;449" SP 'a'     ルビ 1 つ目の位置
	//	  あ い ぞ う
	//	  CSI "390;449" SP 'a'     ルビ 2 つ目の位置
	//	  う ず ま
	//	  CSI "170;509" SP 'a'     本文の位置 (1 行下)
	//	  8A                       NSZ (標準)
	//	  ♬ 愛 憎 愛 憎 渦 巻 い て
	//
	//	  -> ♬ </>愛憎<!>あいぞう</></>渦巻<!>うずま</>いて
	struct RubyRun {
		int Left = 0;
		int Right = 0;
		std::wstring Text;
	};
	std::vector<RubyRun> Rubies;
	bool fRubyOpen = false;		// 直前の文字と同じまとまりか

	//	本文の文字ごとの、Cur.Text の中の位置と字幕平面での X
	struct CharBox {
		size_t Begin;
		size_t End;
		int Left;
		int Right;
	};
	std::vector<CharBox> Boxes;

	//	**これから書く一続きの文字が既に半角かどうか。**
	//	真なら横倍率 <tw> を掛けない。中型 (MSZ) は「横に潰す」ではなく
	//	「半角形を使う」指定で、潰すと `。` の丸が楕円になる (実機で発生)
	bool fRunHalfwidth = false;

	//	行を閉じる直前に呼ぶ。溜めておいたルビを本文に括り付ける。
	//	**後ろから入れる事。**前から入れると後ろの位置がずれる
	auto ApplyRuby = [&]() {
		if (!Rubies.empty() && !Boxes.empty()) {
			std::sort(Rubies.begin(), Rubies.end(),
					  [](const RubyRun &a, const RubyRun &b) {
						  return a.Left < b.Left;
					  });
			for (size_t n = Rubies.size(); n-- > 0; ) {
				const RubyRun &r = Rubies[n];
				//	**その字の半分以上に掛かっていたら含める。**
				//	少しでも重なれば含める形にすると、ルビが本文より
				//	広い時に隣の字まで巻き込む
				//	(実測: 「大東京狂騒歌って」のルビ「きょうそう」が
				//	 5 文字 = 100 ドットあり、狂騒 80 ドットからはみ出した
				//	 20 ドットで「歌」まで括ってしまっていた)。
				//	**ちょうど半分は含める。**外すと 1 文字のルビが
				//	どの字にも掛からず消える (「然(さ)らば」のルビ
				//	「さ」は 20 ドットで、字幅 40 のちょうど半分)。
				//
				//	**この判定では分けられない組がある。**
				//	  渦巻(うずま)  ルビ 60 / 渦 40 + 巻 40 → 巻 は重なり 20
				//	  狂騒(きょうそう) ルビ 100 / 狂 40 + 騒 40 → 歌 も重なり 20
				//	前者は含めたく、後者は含めたくないが、**放送のバイト列は
				//	同じ形** (どちらも左揃えで重なり 20)。ルビが本文より
				//	広い時だけ 1 字余分に括られるが、ルビが消えるよりは良い
				size_t First = Boxes.size(), Last = 0;
				for (size_t k = 0; k < Boxes.size(); k++) {
					const int L = (Boxes[k].Left > r.Left) ? Boxes[k].Left : r.Left;
					const int R = (Boxes[k].Right < r.Right) ? Boxes[k].Right : r.Right;
					const int Width = Boxes[k].Right - Boxes[k].Left;
					if (R > L && (R - L) * 2 >= Width) {
						if (First == Boxes.size())
							First = k;
						Last = k;
					}
				}
				if (First == Boxes.size())
					continue;		// 掛かる字が無い
				Cur.Text.insert(Boxes[Last].End,
								L"<!>" + r.Text + L"</>");
				Cur.Text.insert(Boxes[First].Begin, L"</>");
			}
		}
		Rubies.clear();
		Boxes.clear();
		fRubyOpen = false;
	};

	//	本文を出す直前に呼ぶ。行が変わっていたら改行を入れる
	auto NewLine = [&]() {
		if (PendingY < 0)
			return;
		//	小型 (縦横とも半分) = ルビ。行としては数えない
		if (ScaleH == 5 && ScaleV == 5) {
			PendingY = -1;
			return;
		}

		//	**ACPS が指しているのは行の下端。**1 行分引いて上端に直す
		//	(実測: 行送り 60 で本文 y=509、その 1 行上のルビが y=449)
		const int Top = PendingY + 1 - (PendingPitch > 0 ? PendingPitch : 60);
		const int Threshold = (PendingPitch > 0) ? PendingPitch * 3 / 4 : 1;

		//	**同じ高さで横に飛んだら、そこも別の行にする。**
		//	ドラマやアニメで複数の話者を同時に別の場所へ出す時に起こる。
		//	割らないと離れた場所の文字が 1 つのオブジェクトに繋がり、
		//	間の空白が消えて背景の箱も両方をまたいでしまう
		//	(実測: 放送 14 番組中 4 番組で発生)
		const int GapX = (PendingPitchX > 0) ? PendingPitchX : 40;
		const bool fRow = fAnyText && BaseY >= 0
						  && PendingY - BaseY >= Threshold;
		const bool fGap = fAnyText && BaseY >= 0 && PendingY == BaseY
						  && Cur.Right > 0 && PendingX > Cur.Right + GapX;

		if (fRow || fGap) {
			//	**行が変わった。**ここで区切って別のオブジェクトにする
			ApplyRuby();
			if (!Cur.Text.empty())
				Lines.push_back(Cur);
			Cur = AribCaptionLine();
			//	**行ごとに別のオブジェクトになるので、属性は出し直す。**
			//	大きさだけは「既定」を覚えたままにする。-1 にすると
			//	標準の行の先頭に無駄な <s> が付く
			EmittedColor = -1;
			EmittedBack = -1;
			EmittedH = 10;
			EmittedV = 10;
			fSizeEmitted = false;
		}
		if (Cur.Left < 0 || PendingX < Cur.Left)
			Cur.Left = PendingX;
		if (Cur.Top < 0 || Top < Cur.Top)
			Cur.Top = Top;
		BaseY = PendingY;
		PendingY = -1;
	};

	//	本文を書き足す直前に、行の先頭ならプリセットを入れる
	auto BeginLine = [&]() {
		if (!Cur.Text.empty() || Options.Preset.empty())
			return;
		//	**書体を一括で変える為の指定。**利用者は AviUtl2 側の
		//	テキストプリセットを 1 つ直すだけで全ての字幕の書体を変えられる
		Cur.Text += L"<$";
		Cur.Text += Options.Preset;
		Cur.Text += L">";
	};

	auto Flush = [&]() {
		const bool fColor = Options.UseBroadcastColor && AribColorIsKnown(Color);
		const bool fBack = Options.UseBroadcastBackColor && AribColorIsKnown(Back);
		if ((fColor && Color != EmittedColor) || (fBack && Back != EmittedBack)) {
			//	<#文字色,影縁色>。2 つ目は「背景の箱」ではなく影・縁色で、
			//	テキストプリセット側で文字装飾を縁取りにして初めて見える
			Cur.Text += L"<#";
			if (fColor)
				AppendHex(&Cur.Text, AribColorToRgb(Color));
			if (fBack) {
				Cur.Text += L",";
				AppendHex(&Cur.Text, AribColorToRgb(Back));
			}
			Cur.Text += L">";
			EmittedColor = Color;
			EmittedBack = Back;
		}
		//	**半角に差し替えられた文字には横倍率を掛けない。**
		//	字形がもともと半分の幅なので、更に潰すと歪む
		//	(libaribcaption の needless_horizontal_scaling と同じ判断)
		const int EffH = fRunHalfwidth ? ScaleV : ScaleH;

		if (EffH != EmittedH || ScaleV != EmittedV
				|| (BaseSize > 0 && !fSizeEmitted)) {
			//	**縦のスケールは <s> (文字サイズ) で、横との差だけを
			//	<tw> (横スケール) で出す。**<s> は縦横の両方に効くので、
			//	先に縦を決めてから横の比を掛ける形にしないと合わない。
			//
			//	**<tw> は百分率ではなく倍率。**aviutl2.txt の例が
			//	<tw0.8>、入力補助の既定も <tw0.8> になっている。
			//	<tw50> と書くと 50 倍に引き伸ばされ、文字が横一線に
			//	潰れて画面を横切る (実機で発生)
			if (BaseSize > 0) {
				//	**放送の大きさに合わせる。**絶対値で出すので、
				//	プリセットのサイズは効かなくなる
				Cur.Text += L"<s";
				Cur.Text += std::to_wstring(BaseSize * ScaleV / 10);
				Cur.Text += L">";
			} else if (ScaleV != 10) {
				//	**必ず一度 <s> で戻してから掛ける。**
				//	相対指定が「元の大きさから」なのか「今の大きさから」
				//	なのかが仕様に書かれていない。戻してから掛ければ
				//	どちらの解釈でも同じ結果になる
				Cur.Text += L"<s><s*";
				Cur.Text += ScaleName(ScaleV);
				Cur.Text += L">";
			} else if (EmittedV != 10) {
				Cur.Text += L"<s>";			// 標準に戻す時だけ
			}

			//	横は縦との比。縦横が同じなら <s> で足りている
			if (EffH != ScaleV) {
				Cur.Text += L"<tw";
				Cur.Text += ScaleName(EffH * 10 / ScaleV);
				Cur.Text += L">";
			} else if (EmittedH != EmittedV) {
				Cur.Text += L"<tw>";		// 横だけ伸縮していたのを戻す
			}

			fSizeEmitted = true;
			EmittedH = EffH;
			EmittedV = ScaleV;
		}
	};

	for (const AribItem &it : Items) {
		switch (it.Type) {
		case AribItemType::Text: {
			NewLine();

			//	**小型 (SSZ) はルビ。本文には混ぜない。**
			//	そのまま出すと本文の頭にふりがなだけが並ぶ
			if (Options.UseRuby && ScaleH == 5 && ScaleV == 5 && it.D >= 0) {
				//	X が続いている間は同じまとまり
				if (!fRubyOpen || Rubies.empty()
						|| Rubies.back().Right != it.D) {
					RubyRun r;
					r.Left = it.D;
					r.Right = it.C;
					r.Text = it.Text;
					Rubies.push_back(r);
				} else {
					Rubies.back().Text += it.Text;
					Rubies.back().Right = it.C;
				}
				fRubyOpen = true;
				break;
			}
			fRubyOpen = false;

			BeginLine();

			//	**中型 (MSZ) は「横に潰す」ではなく「半角形を使う」指定。**
			//	`。` を <tw0.5> で潰すと丸が楕円になる (実機で発生)。
			//	半角形のある字はそちらに差し替え、その字には <tw> を
			//	掛けない。半角形の無い字は従来通り横半分に潰す。
			//	1 つの指定の中に両方が混ざる事があるので、
			//	半角になったかどうかで区切って別々に出す
			const bool fMsz = (ScaleH * 2 == ScaleV);
			const std::wstring &s = it.Text;
			size_t n = 0;
			while (n < s.size()) {
				std::wstring Group;
				bool fHalf = false;
				for (bool fFirst = true; n < s.size(); fFirst = false) {
					const WCHAR c = s[n];
					const WCHAR h = fMsz ? ::TSMemoryAribHalfwidth(c) : 0;
					const WCHAR o = (h != 0) ? h : c;
					const bool f = fMsz && ::TSMemoryAribIsHalfwidth(o);
					if (fFirst)
						fHalf = f;
					else if (f != fHalf)
						break;
					Group += o;
					n++;
				}
				fRunHalfwidth = fHalf;
				Flush();
				//	**ルビを括る位置を覚えておく。**属性の制御文字は
				//	この外に出るので、字だけを囲める
				const size_t Begin = Cur.Text.size();
				Cur.Text += Escape(Group);
				if (it.D >= 0 && it.C > it.D) {
					const CharBox Box = { Begin, Cur.Text.size(), it.D, it.C };
					Boxes.push_back(Box);
				}
			}
			fRunHalfwidth = false;

			if (it.C > Cur.Right)
				Cur.Right = it.C;
			fAnyText = true;
			break;
		}

		case AribItemType::LineBreak:
			//	放送はまず送って来ないが、来たら行を切る
			ApplyRuby();
			if (!Cur.Text.empty()) {
				const int Left = Cur.Left;
				Lines.push_back(Cur);
				Cur = AribCaptionLine();
				Cur.Left = Left;
			}
			break;

		case AribItemType::Color:
			Color = it.A;
			break;

		case AribItemType::BackColor:
			Back = it.A;
			break;

		case AribItemType::Size:
			ScaleH = it.A;
			ScaleV = it.B;
			break;

		case AribItemType::Geometry:
			if (it.B > 0) {
				PlaneH = it.B;
				//	横は SWF から決まる。540 なら 960、1080 なら 1920
				PlaneW = it.B * 16 / 9;
			}
			//	字幕平面の何ドット角か、を出力の大きさに直す
			if (Options.UseBroadcastSize && Options.ScreenHeight > 0
					&& it.A > 0 && it.B > 0) {
				const int n = it.A * Options.ScreenHeight / it.B;
				if (n != BaseSize) {
					BaseSize = n;
					fSizeEmitted = false;
				}
			}
			break;

		case AribItemType::Drcs: {
			NewLine();
			//	外字はルビには使われない。本文の 1 字として扱う
			fRubyOpen = false;
			BeginLine();
			Flush();
			const size_t DrcsBegin = Cur.Text.size();
			if (Options.DrcsFont.empty() || pDrcsCodes == nullptr) {
				Cur.Text += Options.DrcsFallback;
				break;
			}
			//	同じ字形は同じ符号に割り当てる
			int Index = -1;
			for (size_t i = 0; i < pDrcsCodes->size(); i++) {
				if ((*pDrcsCodes)[i] == it.A) {
					Index = static_cast<int>(i);
					break;
				}
			}
			if (Index < 0) {
				Index = static_cast<int>(pDrcsCodes->size());
				pDrcsCodes->push_back(it.A);
			}

			//	外字の 1 文字だけフォントを切り替える。
			//	本文の書体はプリセットのまま保たれる
			Cur.Text += L"<@";
			Cur.Text += Options.DrcsFont;
			Cur.Text += L">";
			Cur.Text += static_cast<wchar_t>(Options.DrcsFirstCode + Index);
			Cur.Text += L"<@>";			// 元の書体に戻す
			if (it.D >= 0 && it.C > it.D) {
				const CharBox Box = { DrcsBegin, Cur.Text.size(), it.D, it.C };
				Boxes.push_back(Box);
			}
			if (it.C > Cur.Right)
				Cur.Right = it.C;
			fAnyText = true;
			break;
		}

		case AribItemType::Position:
			//	**ここでは改行しない。**この位置に何が書かれるか
			//	(本文かルビか) を見てから決める必要がある
			PendingX = it.A;
			PendingY = it.B;
			PendingPitch = it.C;
			PendingPitchX = it.D;
			//	**位置が変わればルビのまとまりも変わる**
			fRubyOpen = false;
			break;

		case AribItemType::Ornament:
			//	縁取り。**AviUtl2 側では文字装飾がプリセットの持ち物**で、
			//	制御文字からは切り替えられない。<#文字色,影縁色> の
			//	2 つ目に流す事はできるが、プリセットの文字装飾を
			//	縁取りにしていないと見えない。今は使っていない
			break;

		case AribItemType::ClearScreen:
			//	画面消去は字幕の区切りとして呼び出し側が使う
			break;
		}
	}

	ApplyRuby();
	if (!Cur.Text.empty())
		Lines.push_back(Cur);

	if (pLayout != nullptr) {
		pLayout->Lines = Lines;
		pLayout->PlaneWidth = PlaneW;
		pLayout->PlaneHeight = PlaneH;
	}

	//	戻り値は全部を改行で繋いだ物。確認用と、行ごとに置けない時の保険
	std::wstring Out;
	for (size_t i = 0; i < Lines.size(); i++) {
		if (i > 0)
			Out += L"\n";
		Out += Lines[i].Text;
	}
	return Out;
}
