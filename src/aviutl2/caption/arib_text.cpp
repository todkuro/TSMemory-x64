//----------------------------------------------------------------------------
//	ARIB STD-B24 の 8 単位符号を解く (arib_text.h を参照)
//----------------------------------------------------------------------------
#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#include "arib_text.h"

namespace {

//	文字集合の種類。ESC の終端バイトで指定される
enum class CharSet {
	Kanji,			// 0x42  2 バイト (JIS X 0208)
	Alnum,			// 0x4A  英数
	Hiragana,		// 0x30
	Katakana,		// 0x31
	JisKatakana,	// 0x49  JIS X 0201 片仮名
	AddSymbol,		// 0x3B  追加記号 (2 バイト)
	Drcs1,			// 0x41-0x4F  1 バイト外字
	Drcs2,			// 0x40       2 バイト外字
	Macro,			// 0x70
	Other,
};

bool IsTwoByte(CharSet s)
{
	return s == CharSet::Kanji || s == CharSet::AddSymbol || s == CharSet::Drcs2;
}

CharSet FromFinal(BYTE F, bool fTwoByte)
{
	switch (F) {
	case 0x42: return CharSet::Kanji;
	case 0x4A: return CharSet::Alnum;
	case 0x30: return CharSet::Hiragana;
	case 0x31: return CharSet::Katakana;
	case 0x49: return CharSet::JisKatakana;
	case 0x3B: return CharSet::AddSymbol;
	case 0x39: case 0x3A: return CharSet::Kanji;	// JIS 互換漢字
	case 0x70: return CharSet::Macro;
	case 0x40: return CharSet::Drcs2;
	default:
		if (F >= 0x41 && F <= 0x4F)
			return CharSet::Drcs1;
		return fTwoByte ? CharSet::Kanji : CharSet::Other;
	}
}

//	ひらがな (区 4) / カタカナ (区 5) は JIS X 0208 の該当区に載っている。
//	1 バイトの符号 0x21-0x73 が区の 1 点目から順に並ぶ
const int KU_HIRAGANA = 4;
const int KU_KATAKANA = 5;

//	JIS X 0208 の区点を Shift_JIS の 2 バイトに直す
bool KuTenToSjis(int Ku, int Ten, BYTE *pOut)
{
	if (Ku < 1 || Ku > 94 || Ten < 1 || Ten > 94)
		return false;

	int s1, s2;
	if (Ku <= 62)
		s1 = (Ku + 257) / 2;
	else
		s1 = (Ku + 385) / 2;

	if (Ku & 1) {
		s2 = Ten + 63;
		if (Ten >= 64)
			s2 += 1;
	} else {
		s2 = Ten + 158;
	}
	pOut[0] = static_cast<BYTE>(s1);
	pOut[1] = static_cast<BYTE>(s2);
	return true;
}

//	区点から 1 文字を得る。**追加記号 (区 85-94) は Shift_JIS に無い**ので
//	変換出来ず、空を返す (呼び出し側で外字と同じ扱いにする)
std::wstring KuTenToText(int Ku, int Ten)
{
	BYTE Sjis[2];
	if (!KuTenToSjis(Ku, Ten, Sjis))
		return std::wstring();

	WCHAR w[4] = {};
	const int n = ::MultiByteToWideChar(932, MB_ERR_INVALID_CHARS,
										reinterpret_cast<const char *>(Sjis), 2, w, 4);
	if (n <= 0)
		return std::wstring();
	return std::wstring(w, n);
}

//	JIS X 0201 の片仮名 (半角)
std::wstring JisKatakanaToText(BYTE b)
{
	const char c = static_cast<char>(b | 0x80);
	WCHAR w[2] = {};
	if (::MultiByteToWideChar(932, 0, &c, 1, w, 2) <= 0)
		return std::wstring();
	return std::wstring(w, 1);
}

//---------------------------------------------------------------------------
//	復号の状態
//---------------------------------------------------------------------------
struct State {
	CharSet G[4] = { CharSet::Kanji, CharSet::Alnum,
					 CharSet::Hiragana, CharSet::Katakana };
	int GL = 0;			// GL に呼び出している G の番号
	int GR = 2;
	int Single = -1;	// 単一シフト中なら G の番号

	//	COL で選ばれている色配列 (CLUT) の番号。
	//	色の指定は「この番号 * 16 + 下位ニブル」で 128 色の中を指す
	int ColorMap = 0;
};


//	CSI の引数は 0x30-0x39 と 0x3B が続き、0x20 + 終端バイトで終わる
size_t SkipCsi(const BYTE *p, size_t Size, size_t i)
{
	while (i < Size && ((p[i] >= 0x30 && p[i] <= 0x39) || p[i] == 0x3B))
		i++;
	if (i < Size && p[i] == 0x20)
		i++;
	if (i < Size)
		i++;			// 終端バイト
	return i;
}

void PushText(std::vector<AribItem> *pOut, const std::wstring &s)
{
	if (s.empty())
		return;
	if (!pOut->empty() && pOut->back().Type == AribItemType::Text) {
		pOut->back().Text += s;
		return;
	}
	AribItem it;
	it.Type = AribItemType::Text;
	it.Text = s;
	pOut->push_back(it);
}

void PushSimple(std::vector<AribItem> *pOut, AribItemType Type, int A = 0, int B = 0)
{
	AribItem it;
	it.Type = Type;
	it.A = A;
	it.B = B;
	pOut->push_back(it);
}

//	色の指定を読む。**上位ニブルが前景と背景を分ける。**
//	  0x40+n … 前景色 / 0x50+n … 背景色 / 0x60,0x70 … 半透過
//	区別せず全部を前景色として扱うと、背景色が文字色を上書きしてしまう
//	(実測でも `90 20 44` = 前景 4、`90 51` = 背景 1 と並んでいた)。
size_t PushColor(std::vector<AribItem> *pOut, const BYTE *pData, size_t Size,
				 size_t i, int *pColorMap)
{
	if (i >= Size)
		return i;

	//	**0x20 が続く形は色そのものではなく、色配列 (CLUT) の選択。**
	//	ここを前景色として扱うと、字幕がほぼ全て同じ色に染まる。
	//	実測 (放送 25 番組の字幕文 36 件) では
	//	`90 20 44` (22 件) と `90 20 40` (4 件) の 2 種類しか現れず、
	//	番組をまたいで同じ値だった。話者ごとに変わる色ではない
	if (pData[i] == 0x20) {
		if (i + 1 >= Size)
			return Size;
		if ((pData[i + 1] & 0xF0) == 0x40)
			*pColorMap = pData[i + 1] & 0x0F;
		return i + 2;
	}

	//	上位ニブルが前景 (0x40) か背景 (0x50) かを決める。
	//	**両方を Color にすると背景の指定で文字色が上書きされる。**
	//	下位ニブルは選択中の CLUT の中での番号
	const BYTE v = pData[i];
	const int Base = *pColorMap * 16;
	if (v < 0x10)
		PushSimple(pOut, AribItemType::Color, Base + (v & 0x0F));
	else if ((v & 0xF0) == 0x40)
		PushSimple(pOut, AribItemType::Color, Base + (v & 0x0F));
	else if ((v & 0xF0) == 0x50)
		PushSimple(pOut, AribItemType::BackColor, Base + (v & 0x0F));
	return i + 1;
}

}	// namespace


void AribDecodeText(const BYTE *pData, size_t Size, std::vector<AribItem> *pOut)
{
	if (pData == nullptr || pOut == nullptr)
		return;

	State st;

	for (size_t i = 0; i < Size; ) {
		const BYTE b = pData[i];

		//	--- C0 制御符号 -------------------------------------------------
		if (b <= 0x20) {
			i++;
			switch (b) {
			case 0x0C:	PushSimple(pOut, AribItemType::ClearScreen); break;
			case 0x0D:	PushSimple(pOut, AribItemType::LineBreak); break;	// APR
			case 0x0A:	PushSimple(pOut, AribItemType::LineBreak); break;	// APD
			case 0x0F:	st.GL = 0; break;									// LS0
			case 0x0E:	st.GL = 1; break;									// LS1
			case 0x19:	st.Single = 2; break;								// SS2
			case 0x1D:	st.Single = 3; break;								// SS3
			case 0x16:	i++; break;											// PAPF (1)
			case 0x1C:	// APS (2) : 行, 桁
				if (i + 1 < Size) {
					PushSimple(pOut, AribItemType::Position,
							   pData[i + 1] & 0x3F, pData[i] & 0x3F);
				}
				i += 2;
				break;
			case 0x20:	PushText(pOut, L" "); break;
			case 0x1B: {	// ESC
				if (i >= Size) break;
				const BYTE e = pData[i++];
				if (e == 0x6E) { st.GL = 2; break; }			// LS2
				if (e == 0x6F) { st.GL = 3; break; }			// LS3
				if (e == 0x7E) { st.GR = 1; break; }			// LS1R
				if (e == 0x7D) { st.GR = 2; break; }			// LS2R
				if (e == 0x7C) { st.GR = 3; break; }			// LS3R

				if (e == 0x24) {							// 2 バイト集合
					if (i >= Size) break;
					BYTE f = pData[i++];
					int g = 0;
					if (f >= 0x28 && f <= 0x2B) {
						g = f - 0x28;
						if (i >= Size) break;
						f = pData[i++];
						if (f == 0x20) {					// DRCS
							if (i >= Size) break;
							f = pData[i++];
						}
					}
					st.G[g] = FromFinal(f, true);
					break;
				}
				if (e >= 0x28 && e <= 0x2B) {				// 1 バイト集合
					const int g = e - 0x28;
					if (i >= Size) break;
					BYTE f = pData[i++];
					if (f == 0x20) {						// DRCS
						if (i >= Size) break;
						f = pData[i++];
					}
					st.G[g] = FromFinal(f, false);
					break;
				}
				break;
			}
			default:
				break;
			}
			continue;
		}

		//	--- C1 制御符号 -------------------------------------------------
		if (b >= 0x80 && b <= 0xA0) {
			i++;
			if (b <= 0x87) {						// BKF..WHF (前景色)
				PushSimple(pOut, AribItemType::Color, b - 0x80);
				continue;
			}
			switch (b) {
			case 0x88:	PushSimple(pOut, AribItemType::Size, 2); break;	// SSZ 小型
			case 0x89:	PushSimple(pOut, AribItemType::Size, 1); break;	// MSZ 中型
			case 0x8A:	PushSimple(pOut, AribItemType::Size, 0); break;	// NSZ 標準
			case 0x8B:	// SZX (1)
				if (i < Size)
					PushSimple(pOut, AribItemType::Size, pData[i] == 0x41 ? 3 : 0);
				i++;
				break;
			case 0x8C:	// COL
				i = PushColor(pOut, pData, Size, i, &st.ColorMap);
				break;
			case 0x90:
				//	**実データに合わせている。**
				//	`90 20 44` と `90 51` の両方が現れ、COL (0x8C) と同じ
				//	「0x20 が続けば 2 バイト、そうでなければ 1 バイト」の形。
				//	1 バイト固定で読むと次の文字とずれ、以降が全て化ける。
				//	続く値も 0x40+n / 0x50+n と色指定の並びになっている
				i = PushColor(pOut, pData, Size, i, &st.ColorMap);
				break;

			case 0x8D: case 0x8E: case 0x8F:	// FLC / CDC / POL
			case 0x91: case 0x93:				// MACRO / HLC
			case 0x94:							// RPC
				i++;
				break;

			case 0x95: case 0x96:				// SPL / STL (引数なし)
				break;
			case 0x98:	i += 2; break;			// TIME
			case 0x9B:	i = SkipCsi(pData, Size, i); break;	// CSI
			case 0x9D:	i += 2; break;			// TIME (別符号位置)
			default:
				break;
			}
			continue;
		}

		//	--- 図形文字 ----------------------------------------------------
		const int g = st.Single >= 0 ? st.Single : (b < 0x80 ? st.GL : st.GR);
		st.Single = -1;
		const CharSet Set = st.G[g];
		const BYTE c1 = b & 0x7F;

		if (IsTwoByte(Set)) {
			if (i + 1 >= Size) break;
			const BYTE c2 = pData[i + 1] & 0x7F;
			i += 2;

			if (Set == CharSet::Drcs2) {
				PushSimple(pOut, AribItemType::Drcs, (c1 << 8) | c2);
				continue;
			}
			const int Ku = c1 - 0x20;
			const int Ten = c2 - 0x20;
			const std::wstring s = KuTenToText(Ku, Ten);
			if (s.empty()) {
				//	追加記号など Shift_JIS に無い物。外字と同じ扱いにして
				//	呼び出し側で判断出来るようにする
				PushSimple(pOut, AribItemType::Drcs, (c1 << 8) | c2);
			} else {
				PushText(pOut, s);
			}
			continue;
		}

		i++;
		switch (Set) {
		case CharSet::Alnum: {
			//	英数は ASCII と同じ並び。全角にはしない
			const WCHAR w[2] = { static_cast<WCHAR>(c1), 0 };
			PushText(pOut, w);
			break;
		}
		case CharSet::Hiragana:
			PushText(pOut, KuTenToText(KU_HIRAGANA, c1 - 0x20));
			break;
		case CharSet::Katakana:
			PushText(pOut, KuTenToText(KU_KATAKANA, c1 - 0x20));
			break;
		case CharSet::JisKatakana:
			PushText(pOut, JisKatakanaToText(c1));
			break;
		case CharSet::Drcs1:
			PushSimple(pOut, AribItemType::Drcs, c1);
			break;
		default:
			break;
		}
	}
}


std::wstring AribItemsToPlainText(const std::vector<AribItem> &Items, LPCWSTR pszDrcs)
{
	std::wstring Out;
	for (const AribItem &it : Items) {
		switch (it.Type) {
		case AribItemType::Text:		Out += it.Text; break;
		case AribItemType::Drcs:		Out += pszDrcs; break;
		case AribItemType::LineBreak:	Out += L"\n"; break;
		default:						break;
		}
	}
	return Out;
}
