//----------------------------------------------------------------------------
//	ARIB STD-B24 の 8 単位符号を解く (arib_text.h を参照)
//----------------------------------------------------------------------------
#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#include "arib_text.h"
#include "arib_gaiji.h"

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

//	ESC の終端バイトから文字集合を決める。
//
//	**外字の指定 (ESC ... 0x20 F) は別の表。**同じ終端バイトでも
//	意味が違う。混ぜると外字 2 番 (0x42) が漢字集合に、
//	9 番 (0x49) が JIS X0201 カタカナに、10 番 (0x4A) が英数になる
//	(実測の 33 番組では 0x41 = 外字 1 番しか来ないので表には出ないが、
//	 来たら本文が化ける)
CharSet FromFinal(BYTE F, bool fTwoByte, bool fDrcs)
{
	if (fDrcs) {
		if (F == 0x70) return CharSet::Macro;
		if (F == 0x40) return CharSet::Drcs2;		// 外字 0 番は 2 バイト
		if (F >= 0x41 && F <= 0x4F) return CharSet::Drcs1;
		return CharSet::Other;
	}

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
	//	**プロポーショナル集合。**字形の表は普通の集合と同じで、
	//	送り幅だけが字ごとに変わる。対応していないと `Other` に落ちて
	//	**その集合の文字が丸ごと消える**
	//	(実測の 33 番組では現れないが、来たら本文が欠ける)
	case 0x36: return CharSet::Alnum;
	case 0x37: return CharSet::Hiragana;
	case 0x38: return CharSet::Katakana;
	default:
		return fTwoByte ? CharSet::Kanji : CharSet::Other;
	}
}

//	1 バイトの文字集合は arib_gaiji.h の表で引く。
//	**区 4 / 区 5 で代用してはいけない。**末尾に「ー」「、」等が入っており、
//	区で引くと落ちる (実測: ステーション -> ステション)
std::wstring OneByteToText(WCHAR w)
{
	if (w == 0)
		return std::wstring();
	return std::wstring(1, w);
}

//	区点から 1 文字を得る。無ければ空を返す
//	(呼び出し側で外字と同じ扱いにする)。
//
//	**Shift_JIS (CP932) 経由にしてはいけない。**
//	区 85-94 は ARIB の追加漢字・追加記号で、CP932 の同じ位置には
//	別の文字が載っている。以前は CP932 に通していた為、
//	例えば区 92 点 92 が「釗」になっていた。
//	機械的に数えると、区点 8836 個のうち
//	  214 個が違う文字になり、
//	  1200 個は CP932 に無い為に出せていなかった (♬ 等)。
std::wstring KuTenToText(int Ku, int Ten)
{
	int Length = 0;
	const WCHAR *p = ::TSMemoryAribKuTen(Ku, Ten, &Length);
	if (p == nullptr || Length <= 0)
		return std::wstring();
	return std::wstring(p, Length);
}

//---------------------------------------------------------------------------
//	復号の状態
//---------------------------------------------------------------------------
struct State {
	//	**G3 の初期値はマクロ。**カタカナにしておくと
	//	`SS3 0x61` (マクロ 1 の呼び出し) が「メ」という文字になる。
	//	実測でも NHK の字幕が全て「メ」で始まっていた
	CharSet G[4] = { CharSet::Kanji, CharSet::Alnum,
					 CharSet::Hiragana, CharSet::Macro };
	int GL = 0;			// GL に呼び出している G の番号
	int GR = 2;
	int Single = -1;	// 単一シフト中なら G の番号

	//	COL で選ばれている色配列 (CLUT) の番号。
	//	色の指定は「この番号 * 16 + 下位ニブル」で 128 色の中を指す
	int ColorMap = 0;

	//	字幕平面の作り。CSI で指定されて来る。
	//	既定は SWF 7 (960x540) の標準的な値
	int OrigX = 0, OrigY = 0;		// SDP : 表示領域の左上
	//	SDF : 表示領域の大きさ。**右端で自動的に折り返す。**
	//	読まないと 1 行に繋がる (実測: 平面 960 幅に対し
	//	右端 1230 まで伸びていた)。0 なら字幕平面いっぱい
	int AreaW = 0, AreaH = 0;
	int CharW = 36, CharH = 36;		// SSM : 文字の大きさ
	int SpaceH = 4, SpaceV = 24;	// SHS / SVS : 字間・行間
	int PlaneW = 960, PlaneH = 540;	// SWF : 字幕平面そのものの大きさ

	//	ペンの位置。ACPS / APS で動き、文字を書くと 1 文字分進む。
	//	**PenY は自動改行に要る。**折り返した時に 1 行下の座標を
	//	作らないと、呼び出し側は行が変わった事に気付けない
	int PenX = -1;
	int PenY = -1;

	//	今の文字の大きさ。10 倍した整数で持つ (5 = 半分 / 20 = 倍)。
	//	**縦横を別に持つ。**SZX には「縦だけ 2 倍」「横だけ 2 倍」があり、
	//	1 つの値では表せない。
	//	**APS の行送りはこれで変わる。**小型の行に標準の送りを使うと、
	//	画面の外を指す座標になる (実測: 区切りの行が y=990 になり、
	//	540 の字幕平面をはみ出した)
	int ScaleH = 10;
	int ScaleV = 10;

	//	RPC で指定された「次の文字を繰り返す回数」。-1 なら指定なし、
	//	0 なら行末まで。**1 文字書くと使い切る**
	int Repeat = -1;

	//	1 文字分の送り幅・送り高さ
	int PitchX() const { return (CharW + SpaceH) * ScaleH / 10; }
	int PitchY() const { return (CharH + SpaceV) * ScaleV / 10; }
};


void PushSimple(std::vector<AribItem> *pOut, AribItemType Type,
				int A = 0, int B = 0, int C = 0, int D = 0);

//	CSI の引数は 0x30-0x39 と 0x3B が続き、0x20 + 終端バイトで終わる。
//
//	**位置指定はここに入っている。**放送の字幕は改行 (APD) ではなく
//	ACPS で 1 行ずつ座標を打って来る為、CSI を読み飛ばすと
//	**行が全て繋がって 1 行になる**。
size_t ParseCsi(const BYTE *p, size_t Size, size_t i,
				State *pSt, std::vector<AribItem> *pOut)
{
	int Param[8] = {};
	int Count = 0;
	int Value = 0;
	bool fAny = false;

	while (i < Size && ((p[i] >= 0x30 && p[i] <= 0x39) || p[i] == 0x3B)) {
		if (p[i] == 0x3B) {
			if (Count < 8) Param[Count++] = Value;
			Value = 0;
			fAny = false;
		} else {
			Value = Value * 10 + (p[i] - 0x30);
			fAny = true;
		}
		i++;
	}
	if (fAny && Count < 8)
		Param[Count++] = Value;

	if (i < Size && p[i] == 0x20)
		i++;
	if (i >= Size)
		return Size;

	const BYTE Final = p[i++];
	switch (Final) {
	case 0x5F:		// SDP : 表示領域の左上
		if (Count >= 2) { pSt->OrigX = Param[0]; pSt->OrigY = Param[1]; }
		break;
	case 0x56:		// SDF : 表示領域の大きさ (右端で折り返す)
		if (Count >= 2) { pSt->AreaW = Param[0]; pSt->AreaH = Param[1]; }
		break;
	case 0x53:		// SWF : 表示書式 (字幕平面の大きさ)
		if (Count >= 1) {
			switch (Param[0]) {
			case 5:  pSt->PlaneW = 1920; pSt->PlaneH = 1080; break;
			case 7:  pSt->PlaneW = 960;  pSt->PlaneH = 540;  break;
			case 9:  pSt->PlaneW = 720;  pSt->PlaneH = 480;  break;
			case 11: pSt->PlaneW = 1280; pSt->PlaneH = 720;  break;
			default: break;
			}
			PushSimple(pOut, AribItemType::Geometry, pSt->CharH, pSt->PlaneH);
		}
		break;
	case 0x57:		// SSM : 文字の大きさ
		if (Count >= 2) {
			pSt->CharW = Param[0];
			pSt->CharH = Param[1];
			//	**文字の大きさは字幕平面の大きさと組でないと意味が無い。**
			//	36 ドットが 960x540 なら 1080p では 72 相当になる
			PushSimple(pOut, AribItemType::Geometry, pSt->CharH, pSt->PlaneH);
		}
		break;
	case 0x58:		// SHS : 字間
		if (Count >= 1) pSt->SpaceH = Param[0];
		break;
	case 0x59:		// SVS : 行間
		if (Count >= 1) pSt->SpaceV = Param[0];
		break;
	case 0x63:		// ORN : 文字外縁 (縁取り)
		//	P1 = 0 なし / 1 縁取り。
		//	**縁の色は 1 つの数に詰められている。**
		//	P2 = 色配列 * 100 + 色番号 で、CLUT の索引は
		//	色配列 * 16 + 色番号。100 で割った所を取り違えると
		//	関係の無い色になる
		if (Count >= 1 && Param[0] == 0) {
			PushSimple(pOut, AribItemType::Ornament, 0);
		} else if (Count >= 2 && Param[0] == 1) {
			const int Map = Param[1] / 100;
			const int Num = Param[1] % 100;
			if (Map < 8 && Num < 16)
				PushSimple(pOut, AribItemType::Ornament, 1, Map * 16 + Num);
		}
		break;
	case 0x5B:		// PLD : 半行下
	case 0x5C:		// PLU : 半行上
		//	上付き・下付きや、局によってはルビに使われる。
		//	無視すると本文と同じ行に混ざる。
		//	**半行なので行の区切りにはならない** (変換側の閾値は
		//	行送りの 3/4 なので、半行は同じ行のままになる)
		if (pSt->PenY >= 0) {
			pSt->PenY += (Final == 0x5B ? 1 : -1) * pSt->PitchY() / 2;
			PushSimple(pOut, AribItemType::Position, pSt->PenX, pSt->PenY,
					   pSt->PitchY(), pSt->PitchX());
		}
		break;
	case 0x61:		// ACPS : 表示位置 (ドット)
		if (Count >= 2) {
			pSt->PenX = Param[0];
			pSt->PenY = Param[1];
			PushSimple(pOut, AribItemType::Position, Param[0], Param[1],
					   pSt->PitchY(), pSt->PitchX());
		}
		break;
	default:
		break;
	}
	return i;
}

//	**1 文字ずつ別の項目にする。まとめてはいけない。**
//	まとめると 1 文字ごとの X が判らなくなり、ルビをどの字に
//	掛けるかを決められない。変換後の文字列は繋げるだけなので変わらない
void PushText(std::vector<AribItem> *pOut, const std::wstring &s,
			  int Left = -1, int Right = -1)
{
	if (s.empty())
		return;
	AribItem it;
	it.Type = AribItemType::Text;
	it.Text = s;
	it.C = Right;		// 書き終えた後のペンの X
	it.D = Left;		// 書き始めた時のペンの X
	pOut->push_back(it);
}

void PushSimple(std::vector<AribItem> *pOut, AribItemType Type,
				int A, int B, int C, int D)
{
	AribItem it;
	it.Type = Type;
	it.A = A;
	it.B = B;
	it.C = C;
	it.D = D;
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
			*pColorMap = pData[i + 1] & 0x07;	// 色配列は 8 個 (3 ビット)
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


namespace {

//	Depth はマクロの入れ子を止める為の物。
//	既定のマクロは ESC の並びしか持たないので 1 段で足りるが、
//	壊れたデータで無限に潜らないようにしておく
void DecodeBody(const BYTE *pData, size_t Size, State &st,
				std::vector<AribItem> *pOut, int Depth);

}	// namespace


void AribDecodeText(const BYTE *pData, size_t Size, std::vector<AribItem> *pOut)
{
	if (pData == nullptr || pOut == nullptr)
		return;

	State st;
	DecodeBody(pData, Size, st, pOut, 0);
}


namespace {

void DecodeBody(const BYTE *pData, size_t Size, State &st,
				std::vector<AribItem> *pOut, int Depth)
{

	//	**表示領域の右端に来たら折り返す。**
	//	放送は 1 行に収まらない字幕をこれで 2 行にしている。
	//	読まないと 1 行に繋がったまま画面をはみ出す
	//	(実測: 平面 960 幅に対して右端が 1230 になっていた)。
	//	文字を書く直前に呼ぶ事
	//	ペンを桁・行の単位で動かす。fHome なら桁を表示領域の左端へ戻す。
	//	座標が判っていない間は改行として伝えるしかない
	auto MovePen = [&](int Cols, int Rows, bool fHome) {
		if (st.PenX < 0 || st.PenY < 0) {
			if (Rows > 0)
				PushSimple(pOut, AribItemType::LineBreak);
			return;
		}
		st.PenX = fHome ? st.OrigX : st.PenX + Cols * st.PitchX();
		st.PenY += Rows * st.PitchY();
		PushSimple(pOut, AribItemType::Position, st.PenX, st.PenY,
				   st.PitchY(), st.PitchX());
	};

	auto Wrap = [&]() {
		if (st.PenX < 0 || st.PenY < 0)
			return;
		const int Width = (st.AreaW > 0) ? st.AreaW : st.PlaneW;
		//	ちょうど収まる字は折り返さない
		if (st.PenX + st.PitchX() <= st.OrigX + Width)
			return;
		st.PenX = st.OrigX;
		st.PenY += st.PitchY();
		PushSimple(pOut, AribItemType::Position, st.PenX, st.PenY,
				   st.PitchY(), st.PitchX());
	};

	//	RPC で「次の文字を繰り返す」と指定されていたら、その回数を返す。
	//	**0 は行末まで。**caption.dll (TVCaptionMod2 が使っている物) が
	//	  m_wRPC = P1 - 0x40 とし、0 の時は右端まで詰めてから改行する。
	//	壊れたデータで暴走しないよう上限を設ける
	auto RepeatCount = [&]() {
		if (st.Repeat < 0)
			return 1;
		const int Times = st.Repeat;
		st.Repeat = -1;						// 1 文字で使い切る
		if (Times > 0)
			return (Times > 512) ? 512 : Times;

		const int Width = (st.AreaW > 0) ? st.AreaW : st.PlaneW;
		if (st.PenX < 0 || st.PitchX() <= 0)
			return 1;
		const int Room = (st.OrigX + Width - st.PenX) / st.PitchX();
		return (Room < 1) ? 1 : ((Room > 512) ? 512 : Room);
	};

	//	文字を 1 つ書く (折り返しと繰り返しの面倒を見る)
	auto PutText = [&](const std::wstring &s) {
		if (s.empty())
			return;
		const int Times = RepeatCount();
		for (int k = 0; k < Times; k++) {
			Wrap();
			const int Left = st.PenX;
			if (st.PenX >= 0) st.PenX += st.PitchX();
			PushText(pOut, s, Left, st.PenX);
		}
	};

	//	外字を 1 つ書く
	auto PutDrcs = [&](int Code) {
		const int Times = RepeatCount();
		for (int k = 0; k < Times; k++) {
			Wrap();
			const int Left = st.PenX;
			if (st.PenX >= 0) st.PenX += st.PitchX();
			PushSimple(pOut, AribItemType::Drcs, Code, 0, st.PenX, Left);
		}
	};

	for (size_t i = 0; i < Size; ) {
		const BYTE b = pData[i];

		//	--- C0 制御符号 -------------------------------------------------
		if (b <= 0x20) {
			i++;
			switch (b) {
			case 0x0C:	PushSimple(pOut, AribItemType::ClearScreen); break;
			//	**ペンを動かす符号。**実測の 33 番組では一度も来ないが、
			//	来た時に位置がずれると本文の並びが崩れる。
			//	座標が判らない時だけ改行として伝える
			case 0x0D:	MovePen(0, 1, true);  break;	// APR 改行 (左端 + 1 行下)
			case 0x0A:	MovePen(0, 1, false); break;	// APD 1 行下 (桁はそのまま)
			case 0x0B:	MovePen(0, -1, false); break;	// APU 1 行上
			case 0x09:	MovePen(1, 0, false); break;	// APF 1 つ進む
			case 0x08:	MovePen(-1, 0, false); break;	// APB 1 つ戻る
			case 0x0F:	st.GL = 0; break;									// LS0
			case 0x0E:	st.GL = 1; break;									// LS1
			case 0x19:	st.Single = 2; break;								// SS2
			case 0x1D:	st.Single = 3; break;								// SS3
			case 0x16:	// PAPF (1) : n つ進む
				//	**引数を捨てるだけでは足りない。**送りに反映しないと
				//	その分だけ以降の位置が左にずれる
				if (i < Size)
					MovePen(pData[i] & 0x3F, 0, false);
				i++;
				break;
			case 0x1C:	// APS (2) : 行, 桁
				//	**ACPS と単位を揃えてドットで持つ。**
				//	混ざったまま渡すと呼び出し側が区別できない
				if (i + 1 < Size) {
					const int Row = pData[i] & 0x3F;
					const int Col = pData[i + 1] & 0x3F;
					st.PenX = st.OrigX + Col * st.PitchX();
					st.PenY = st.OrigY + (Row + 1) * st.PitchY();
					PushSimple(pOut, AribItemType::Position,
							   st.PenX,
							   st.PenY,
							   st.PitchY(), st.PitchX());
				}
				i += 2;
				break;
			case 0x20:
				//	**空白も 1 文字分の枠を占める。**半角の空白にすると
				//	送り幅と合わず、その行だけ詰まって見える。
				//	中型の時は変換側が半角の空白に差し替える
				PutText(L"　");
				break;
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
					bool fDrcs = false;
					if (f >= 0x28 && f <= 0x2B) {
						g = f - 0x28;
						if (i >= Size) break;
						f = pData[i++];
						if (f == 0x20) {					// DRCS
							if (i >= Size) break;
							f = pData[i++];
							fDrcs = true;
						}
					}
					st.G[g] = FromFinal(f, true, fDrcs);
					break;
				}
				if (e >= 0x28 && e <= 0x2B) {				// 1 バイト集合
					const int g = e - 0x28;
					if (i >= Size) break;
					BYTE f = pData[i++];
					bool fDrcs = false;
					if (f == 0x20) {						// DRCS
						if (i >= Size) break;
						f = pData[i++];
						fDrcs = true;
					}
					st.G[g] = FromFinal(f, false, fDrcs);
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
			//	SSZ 小型 = 縦横半分 / MSZ 中型 = 横だけ半分 / NSZ 標準
			case 0x88:	st.ScaleH = 5;  st.ScaleV = 5;
						PushSimple(pOut, AribItemType::Size, 5, 5); break;
			case 0x89:	st.ScaleH = 5;  st.ScaleV = 10;
						PushSimple(pOut, AribItemType::Size, 5, 10); break;
			case 0x8A:	st.ScaleH = 10; st.ScaleV = 10;
						PushSimple(pOut, AribItemType::Size, 10, 10); break;
			case 0x8B:	// SZX (1)
				//	**0x41 は「縦だけ 2 倍」。**縦横 2 倍は 0x45。
				//	0x41 を縦横 2 倍にすると横に伸び過ぎる。
				//	規定外の値では大きさを変えない
				if (i < Size) {
					switch (pData[i]) {
					case 0x41: st.ScaleV = 20; break;			// 縦だけ 2 倍
					case 0x44: st.ScaleH = 20; break;			// 横だけ 2 倍
					case 0x45: st.ScaleH = 20; st.ScaleV = 20; break;
					default:   break;
					}
					PushSimple(pOut, AribItemType::Size, st.ScaleH, st.ScaleV);
				}
				i++;
				break;
			//	**引数の数を 1 つでも間違えると、以降が全て化ける。**
			//	`0x8C..0x8F` は 8 単位符号では未割り当てで、COL は 0x90。
			//	実データの `90 20 44` / `90 51` もこの割り当てと合う
			case 0x90:	// COL (1、0x20 が続けば 2)
				i = PushColor(pOut, pData, Size, i, &st.ColorMap);
				break;

			case 0x92:	// CDC (1、0x20 が続けば 2)
				if (i < Size && pData[i] == 0x20)
					i += 2;
				else
					i++;
				break;

			case 0x91:	// FLC (1)
			case 0x93:	// POL (1)
			case 0x94:	// WMM (1)
			case 0x95:	// MACRO (1)
			case 0x97:	// HLC (1)。**囲み。消費しないと引数が本文に混ざる**
				i++;
				break;

			case 0x98:	// RPC (1) : 次の文字を繰り返す
				//	**回数は P1 - 0x40。0 なら行末まで。**
				//	caption.dll (TVCaptionMod2 が使っている物) と同じ。
				//	無視すると 1 文字しか出ず、その行の以降の位置も
				//	繰り返した分だけ左にずれる
				if (i < Size) {
					const int n = pData[i] - 0x40;
					st.Repeat = (n >= 0) ? n : -1;
				}
				i++;
				break;

			case 0x99: case 0x9A:				// SPL / STL (引数なし)
				break;
			case 0x9B:	i = ParseCsi(pData, Size, i, &st, pOut); break;	// CSI
			case 0x9D:	i += 2; break;			// TIME (2)
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
				PutDrcs((c1 << 8) | c2);
				continue;
			}
			const std::wstring s = KuTenToText(c1 - 0x20, c2 - 0x20);
			if (s.empty()) {
				//	表に無い区点。外字と同じ扱いにして
				//	呼び出し側で判断出来るようにする
				PutDrcs((c1 << 8) | c2);
			} else {
				PutText(s);
			}
			continue;
		}

		i++;
		switch (Set) {
		case CharSet::Alnum:
			//	**英数は全角。**中型 (MSZ) の時は変換側が半角に差し替える
			PutText(OneByteToText(::TSMemoryAribAlnum(c1)));
			break;
		case CharSet::Hiragana:
			PutText(OneByteToText(::TSMemoryAribHiragana(c1)));
			break;
		case CharSet::Katakana:
			PutText(OneByteToText(::TSMemoryAribKatakana(c1)));
			break;
		case CharSet::JisKatakana:
			//	**この文字集合だけは中型 (MSZ) で丸ごと半角に写す。**
			//	もともと半角の片仮名の集合であり、全角の片仮名集合
			//	(ESC 0x31) とは扱いが違う。全角のまま <tw0.5> で
			//	潰すと字形が歪む。libaribcaption も同じ分け方をしている
			PutText(OneByteToText(st.ScaleH * 2 == st.ScaleV
								  ? ::TSMemoryAribJisKatakanaHalf(c1)
								  : ::TSMemoryAribJisKatakana(c1)));
			break;
		case CharSet::Drcs1:
			PutDrcs(c1);
			break;
		case CharSet::Macro: {
			//	**マクロは文字ではない。**中身は文字集合を割り当てる
			//	ESC の並びなので、そのまま読ませて状態だけ変える
			if (c1 < 0x60 || c1 > 0x6F || Depth >= 4)
				break;
			int Len = 0;
			const BYTE *p = ::TSMemoryAribDefaultMacro(c1 & 0x0F, &Len);
			if (p != nullptr && Len > 0)
				DecodeBody(p, static_cast<size_t>(Len), st, pOut, Depth + 1);
			break;
		}
		default:
			break;
		}
	}
}

}	// namespace


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
