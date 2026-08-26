//----------------------------------------------------------------------------
//	復号した字幕を AviUtl2 のテキストに直す (arib_to_aviutl2.h を参照)
//----------------------------------------------------------------------------
#include <windows.h>

#include <string>
#include <cwchar>
#include <vector>

#include "arib_to_aviutl2.h"

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
	return Index >= 0 && Index < 16;
}


DWORD AribColorToRgb(int Index)
{
	//	既定の CLUT の前半 8 色。後半 8 色は半輝度
	static const DWORD Base[8] = {
		0x000000,	// 0 黒
		0xFF0000,	// 1 赤
		0x00FF00,	// 2 緑
		0xFFFF00,	// 3 黄
		0x0000FF,	// 4 青
		0xFF00FF,	// 5 マゼンタ
		0x00FFFF,	// 6 シアン
		0xFFFFFF,	// 7 白
	};

	if (Index < 0)
		Index = 0;
	const DWORD c = Base[Index & 7];
	if ((Index & 8) == 0)
		return c;

	//	半輝度
	return ((c >> 1) & 0x7F7F7F);
}


std::wstring AribItemsToAviUtl2(const std::vector<AribItem> &Items,
								const AribToAviUtl2Options &Options,
								std::vector<int> *pDrcsCodes)
{
	std::wstring Out;

	//	**書体を一括で変える為の指定。**
	//	これを入れておくと、利用者は AviUtl2 側のテキストプリセットを
	//	1 つ直すだけで全ての字幕の書体を変えられる
	if (!Options.Preset.empty()) {
		Out += L"<$";
		Out += Options.Preset;
		Out += L">";
	}

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
		const int Threshold = (PendingPitch > 0) ? PendingPitch * 3 / 4 : 1;
		if (fAnyText && BaseY >= 0 && PendingY - BaseY >= Threshold)
			Out += L"\n";
		BaseY = PendingY;
		PendingY = -1;
	};

	auto Flush = [&]() {
		const bool fColor = Options.UseBroadcastColor && AribColorIsKnown(Color);
		const bool fBack = Options.UseBroadcastBackColor && AribColorIsKnown(Back);
		if ((fColor && Color != EmittedColor) || (fBack && Back != EmittedBack)) {
			//	<#文字色,影縁色>。2 つ目は「背景の箱」ではなく影・縁色で、
			//	テキストプリセット側で文字装飾を縁取りにして初めて見える
			Out += L"<#";
			if (fColor)
				AppendHex(&Out, AribColorToRgb(Color));
			if (fBack) {
				Out += L",";
				AppendHex(&Out, AribColorToRgb(Back));
			}
			Out += L">";
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
				Out += L"<s";
				Out += std::to_wstring(n);
				Out += L">";
				if (Size == 1)
					Out += L"<tw50>";
				else if (EmittedSize == 1)
					Out += L"<tw>";
				fSizeEmitted = true;
			} else {
				switch (Size) {
				case 1:  Out += L"<tw50>"; break;	// 横だけ縮める
				case 2:  Out += L"<s*0.5>"; break;
				case 3:  Out += L"<s*2>"; break;
				default:
					//	既定に戻す。横倍率も戻す
					Out += (EmittedSize == 1) ? L"<tw>" : L"<s>";
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
			Flush();
			Out += Escape(it.Text);
			fAnyText = true;
			break;

		case AribItemType::LineBreak:
			Out += L"\n";
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
			Flush();
			if (Options.DrcsFont.empty() || pDrcsCodes == nullptr) {
				Out += Options.DrcsFallback;
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
			Out += L"<@";
			Out += Options.DrcsFont;
			Out += L">";
			Out += static_cast<wchar_t>(Options.DrcsFirstCode + Index);
			Out += L"<@>";			// 元の書体に戻す
			fAnyText = true;
			break;
		}

		case AribItemType::Position:
			//	**ここでは改行しない。**この位置に何が書かれるか
			//	(本文かルビか) を見てから決める必要がある
			PendingY = it.B;
			PendingPitch = it.C;
			break;

		case AribItemType::ClearScreen:
			//	画面消去は字幕の区切りとして呼び出し側が使う
			break;
		}
	}

	return Out;
}
