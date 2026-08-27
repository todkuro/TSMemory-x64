//----------------------------------------------------------------------------
//	復号した字幕を AviUtl2 のテキストに直す (arib_to_aviutl2.h を参照)
//----------------------------------------------------------------------------
#include <windows.h>

#include <string>
#include <cwchar>
#include <vector>

#include "arib_to_aviutl2.h"
#include "arib_gaiji.h"

namespace {

//	制御文字として解釈される文字を打ち消す。
//	**本文に '<' が含まれると制御文字と誤解される。**
//	字幕には「<笑い>」のような表記が実際に出てくる
std::wstring Escape(const std::wstring &s)
{
	std::wstring Out;
	for (wchar_t c : s) {
		if (c == L'<')
			Out += L"<<";		// AviUtl2 は << を '<' と解釈する
		else
			Out += c;
	}
	return Out;
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
	bool fAnyText = false;

	int Color = -1, EmittedColor = -1;
	int Back = -1, EmittedBack = -1;
	int Size = 0, EmittedSize = 0;

	//	放送の文字の大きさ (AviUtl2 のサイズに直した値)。0 ならプリセット任せ
	int BaseSize = 0;
	bool fSizeEmitted = false;

	//	本文を出す直前に呼ぶ。行が変わっていたら改行を入れる
	auto NewLine = [&]() {
		if (PendingY < 0)
			return;
		if (Size == 2) {		// 小型 = ルビ。行としては数えない
			PendingY = -1;
			return;
		}

		//	**ACPS が指しているのは行の下端。**1 行分引いて上端に直す
		//	(実測: 行送り 60 で本文 y=509、その 1 行上のルビが y=449)
		const int Top = PendingY + 1 - (PendingPitch > 0 ? PendingPitch : 60);
		const int Threshold = (PendingPitch > 0) ? PendingPitch * 3 / 4 : 1;
		if (fAnyText && BaseY >= 0 && PendingY - BaseY >= Threshold) {
			//	**行が変わった。**ここで区切って別のオブジェクトにする
			if (!Cur.Text.empty())
				Lines.push_back(Cur);
			Cur = AribCaptionLine();
			//	**行ごとに別のオブジェクトになるので、属性は出し直す。**
			//	大きさだけは「既定」を覚えたままにする。-1 にすると
			//	標準の行の先頭に無駄な <s> が付く
			EmittedColor = -1;
			EmittedBack = -1;
			EmittedSize = 0;
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
		if (Size != EmittedSize || (BaseSize > 0 && !fSizeEmitted)) {
			//	0 標準 / 1 中型 (横半分) / 2 小型 (縦横半分) / 3 倍角
			if (BaseSize > 0) {
				//	**放送の大きさに合わせる。**相対指定ではなく
				//	絶対値で出すので、プリセットのサイズは効かなくなる
				int n = BaseSize;
				if (Size == 2)
					n = (BaseSize + 1) / 2;
				else if (Size == 3)
					n = BaseSize * 2;
				Cur.Text += L"<s";
				Cur.Text += std::to_wstring(n);
				Cur.Text += L">";
				if (Size == 1)
					Cur.Text += L"<tw50>";
				else if (EmittedSize == 1)
					Cur.Text += L"<tw>";
				fSizeEmitted = true;
			} else {
				switch (Size) {
				case 1:  Cur.Text += L"<tw50>"; break;	// 横だけ縮める
				case 2:  Cur.Text += L"<s*0.5>"; break;
				case 3:  Cur.Text += L"<s*2>"; break;
				default:
					//	既定に戻す。横倍率も戻す
					Cur.Text += (EmittedSize == 1) ? L"<tw>" : L"<s>";
					break;
				}
			}
			EmittedSize = Size;
		}
	};

	for (const AribItem &it : Items) {
		switch (it.Type) {
		case AribItemType::Text:
			NewLine();
			BeginLine();
			Flush();
			Cur.Text += Escape(it.Text);
			if (it.C > Cur.Right)
				Cur.Right = it.C;
			fAnyText = true;
			break;

		case AribItemType::LineBreak:
			//	放送はまず送って来ないが、来たら行を切る
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
			Size = it.A;
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
			BeginLine();
			Flush();
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
			break;

		case AribItemType::ClearScreen:
			//	画面消去は字幕の区切りとして呼び出し側が使う
			break;
		}
	}

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
