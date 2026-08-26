//----------------------------------------------------------------------------
//	外字 (DRCS) のビットマップから TrueType フォントを組み立てる
//	(drcs_ttf.h を参照)
//----------------------------------------------------------------------------
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "drcs_ttf.h"

namespace {

//	字面の大きさ。1024 にすると 36 画素で割っても
//	画素の境目が整数に揃う (端数は丸めるが、隣り合う矩形は同じ値になる)
const int UNITS_PER_EM = 1024;

//	和文の字面に合わせて、ベースラインより少し下まで使う
const int ASCENT = 896;
const int DESCENT = 128;		// 正の値で持つ (下方向)

//	この濃さ以上を「塗る」とみなす。
//	4 階調 (0〜3) なら 2 以上。放送側のアンチエイリアスは捨て、
//	拡大時の滑らかさは DirectWrite に任せる
int FillThreshold(int Depth)
{
	const int Levels = Depth + 2;
	return Levels <= 2 ? 1 : (Levels + 1) / 2;
}

//---------------------------------------------------------------------------
//	バイト列の組み立て (TrueType は全てビッグエンディアン)
//---------------------------------------------------------------------------
struct Writer {
	std::vector<BYTE> Data;

	void U8(unsigned v) { Data.push_back(static_cast<BYTE>(v)); }
	void U16(unsigned v) { U8(v >> 8); U8(v); }
	void U32(uint32_t v) { U16(v >> 16); U16(v & 0xFFFF); }
	void I16(int v) { U16(static_cast<unsigned>(v) & 0xFFFF); }
	void Raw(const void *p, size_t n)
	{
		const BYTE *b = static_cast<const BYTE *>(p);
		Data.insert(Data.end(), b, b + n);
	}
	void Pad4() { while (Data.size() % 4) U8(0); }
	size_t Size() const { return Data.size(); }
};

uint32_t CheckSum(const BYTE *p, size_t Size)
{
	uint32_t Sum = 0;
	size_t i = 0;
	for (; i + 4 <= Size; i += 4)
		Sum += (p[i] << 24) | (p[i + 1] << 16) | (p[i + 2] << 8) | p[i + 3];
	if (i < Size) {
		uint32_t Last = 0;
		for (int k = 0; k < 4; k++)
			Last = (Last << 8) | (i + k < Size ? p[i + k] : 0);
		Sum += Last;
	}
	return Sum;
}

//---------------------------------------------------------------------------
//	字形の輪郭
//---------------------------------------------------------------------------
struct Rect { int x0, y0, x1, y1; };		// 画素単位

//	横に連続する画素を 1 つの矩形にまとめる。
//	36x36 を 1 画素ずつ出すと点が 5000 個を超えるので、行ごとに繋ぐ
std::vector<Rect> ToRects(const TSMemoryDrcsGlyph &Glyph)
{
	std::vector<Rect> Out;
	const int Threshold = FillThreshold(Glyph.Depth);

	for (int y = 0; y < Glyph.Height; y++) {
		int x = 0;
		while (x < Glyph.Width) {
			if (TSMemoryDrcsPixel(Glyph, x, y) < Threshold) {
				x++;
				continue;
			}
			const int Start = x;
			while (x < Glyph.Width && TSMemoryDrcsPixel(Glyph, x, y) >= Threshold)
				x++;
			Out.push_back({ Start, y, x, y + 1 });
		}
	}
	return Out;
}

//	画素の座標をフォントの座標に直す。
//	隣り合う画素で境目の値が一致するよう、同じ式で丸める
int MapX(int px, int Width)
{
	return static_cast<int>((static_cast<int64_t>(px) * UNITS_PER_EM + Width / 2) / Width);
}

int MapY(int py, int Height)
{
	//	ビットマップは上から下、フォントは下から上
	const int64_t Total = ASCENT + DESCENT;
	const int v = static_cast<int>((static_cast<int64_t>(py) * Total + Height / 2) / Height);
	return ASCENT - v;
}

//	1 字分の glyf を作る。空なら何も書かない (loca が同じ値になる)
void BuildGlyf(const TSMemoryDrcsGlyph &Glyph, Writer *pOut)
{
	const std::vector<Rect> Rects = ToRects(Glyph);
	if (Rects.empty())
		return;

	int MinX = UNITS_PER_EM, MinY = ASCENT, MaxX = 0, MaxY = -DESCENT;
	for (const Rect &r : Rects) {
		MinX = std::min(MinX, MapX(r.x0, Glyph.Width));
		MaxX = std::max(MaxX, MapX(r.x1, Glyph.Width));
		MinY = std::min(MinY, MapY(r.y1, Glyph.Height));
		MaxY = std::max(MaxY, MapY(r.y0, Glyph.Height));
	}

	pOut->I16(static_cast<int>(Rects.size()));		// numberOfContours
	pOut->I16(MinX); pOut->I16(MinY);
	pOut->I16(MaxX); pOut->I16(MaxY);

	//	endPtsOfContours
	for (size_t i = 0; i < Rects.size(); i++)
		pOut->U16(static_cast<unsigned>(i * 4 + 3));

	pOut->U16(0);									// instructionLength

	//	flags (全て on-curve。繰り返し圧縮はしない)
	for (size_t i = 0; i < Rects.size() * 4; i++)
		pOut->U8(0x01);

	//	x 座標 (前の点からの差分)
	int Prev = 0;
	for (const Rect &r : Rects) {
		const int x0 = MapX(r.x0, Glyph.Width);
		const int x1 = MapX(r.x1, Glyph.Width);
		//	時計回り (y 上向きの座標系での外周)
		const int xs[4] = { x0, x0, x1, x1 };
		for (int k = 0; k < 4; k++) { pOut->I16(xs[k] - Prev); Prev = xs[k]; }
	}

	//	y 座標
	Prev = 0;
	for (const Rect &r : Rects) {
		const int y0 = MapY(r.y1, Glyph.Height);	// 下
		const int y1 = MapY(r.y0, Glyph.Height);	// 上
		const int ys[4] = { y0, y1, y1, y0 };
		for (int k = 0; k < 4; k++) { pOut->I16(ys[k] - Prev); Prev = ys[k]; }
	}

	pOut->Pad4();
}

//---------------------------------------------------------------------------
//	name テーブル
//---------------------------------------------------------------------------
void BuildName(LPCWSTR pszFamily, Writer *pOut)
{
	//	Windows / Unicode BMP / 英語 (米国) の 1 組だけ入れる。
	//	DirectWrite はこれで読める
	const int IDs[] = { 1, 2, 3, 4, 6 };	// family / subfamily / unique / full / postscript
	const int Count = static_cast<int>(sizeof(IDs) / sizeof(IDs[0]));

	std::vector<std::vector<BYTE>> Strings;
	for (int i = 0; i < Count; i++) {
		LPCWSTR src = pszFamily;
		if (IDs[i] == 2)
			src = L"Regular";
		std::vector<BYTE> s;
		for (LPCWSTR p = src; *p != L'\0'; p++) {
			s.push_back(static_cast<BYTE>(*p >> 8));
			s.push_back(static_cast<BYTE>(*p & 0xFF));
		}
		Strings.push_back(s);
	}

	pOut->U16(0);					// format
	pOut->U16(Count);
	pOut->U16(6 + 12 * Count);		// stringOffset

	int Offset = 0;
	for (int i = 0; i < Count; i++) {
		pOut->U16(3); pOut->U16(1); pOut->U16(0x0409);
		pOut->U16(IDs[i]);
		pOut->U16(static_cast<unsigned>(Strings[i].size()));
		pOut->U16(Offset);
		Offset += static_cast<int>(Strings[i].size());
	}
	for (const auto &s : Strings)
		pOut->Raw(s.data(), s.size());
	pOut->Pad4();
}

//---------------------------------------------------------------------------
//	cmap (format 4)
//---------------------------------------------------------------------------
void BuildCmap(const std::vector<TSMemoryDrcsGlyph> &Glyphs, Writer *pOut)
{
	//	連続する符号を 1 区間にまとめる
	struct Seg { wchar_t Start, End; int Glyph; };
	std::vector<Seg> Segs;
	for (size_t i = 0; i < Glyphs.size(); i++) {
		const wchar_t c = Glyphs[i].Code;
		const int g = static_cast<int>(i) + 1;		// 0 は .notdef
		if (!Segs.empty() && Segs.back().End + 1 == c
				&& Segs.back().Glyph + (Segs.back().End - Segs.back().Start) + 1 == g) {
			Segs.back().End = c;
		} else {
			Segs.push_back({ c, c, g });
		}
	}
	Segs.push_back({ 0xFFFF, 0xFFFF, 0 });			// 必須の終端区間

	const int SegCount = static_cast<int>(Segs.size());
	const int SubLen = 16 + 8 * SegCount;

	pOut->U16(0);					// version
	pOut->U16(1);					// numTables
	pOut->U16(3); pOut->U16(1);		// Windows / Unicode BMP
	pOut->U32(12);					// offset

	pOut->U16(4);					// format
	pOut->U16(SubLen);
	pOut->U16(0);					// language

	int SearchRange = 2;
	int Entry = 0;
	while (SearchRange * 2 <= SegCount * 2) { SearchRange *= 2; Entry++; }
	pOut->U16(SegCount * 2);
	pOut->U16(SearchRange);
	pOut->U16(Entry);
	pOut->U16(SegCount * 2 - SearchRange);

	for (const Seg &s : Segs) pOut->U16(s.End);
	pOut->U16(0);					// reservedPad
	for (const Seg &s : Segs) pOut->U16(s.Start);
	//	idDelta : glyph = code + delta
	for (const Seg &s : Segs) {
		if (s.Start == 0xFFFF) { pOut->I16(1); continue; }
		pOut->I16(static_cast<int16_t>(s.Glyph - s.Start));
	}
	for (int i = 0; i < SegCount; i++) pOut->U16(0);		// idRangeOffset
	pOut->Pad4();
}

}	// namespace


int TSMemoryDrcsPixel(const TSMemoryDrcsGlyph &Glyph, int x, int y)
{
	if (x < 0 || y < 0 || x >= Glyph.Width || y >= Glyph.Height)
		return 0;

	const int Levels = Glyph.Depth + 2;
	int Bits = 1;
	while ((1 << Bits) < Levels)
		Bits++;

	const size_t Index = static_cast<size_t>(y) * Glyph.Width + x;
	const size_t BitPos = Index * Bits;
	const size_t Byte = BitPos / 8;
	if (Byte >= Glyph.Pattern.size())
		return 0;

	const int Shift = 8 - Bits - static_cast<int>(BitPos % 8);
	if (Shift < 0)
		return 0;								// バイトをまたぐ幅は使わない
	return (Glyph.Pattern[Byte] >> Shift) & ((1 << Bits) - 1);
}


bool TSMemoryBuildDrcsFont(const std::vector<TSMemoryDrcsGlyph> &Glyphs,
						   LPCWSTR pszFamily, std::vector<BYTE> *pOut)
{
	if (pOut == nullptr || pszFamily == nullptr || Glyphs.empty())
		return false;

	const int NumGlyphs = static_cast<int>(Glyphs.size()) + 1;	// + .notdef

	//	--- glyf / loca -----------------------------------------------------
	Writer Glyf;
	std::vector<uint32_t> Loca;
	Loca.push_back(0);
	Glyf.Pad4();					// .notdef は空
	Loca.push_back(static_cast<uint32_t>(Glyf.Size()));
	for (const TSMemoryDrcsGlyph &g : Glyphs) {
		BuildGlyf(g, &Glyf);
		Loca.push_back(static_cast<uint32_t>(Glyf.Size()));
	}

	Writer LocaW;
	for (uint32_t v : Loca) LocaW.U32(v);
	LocaW.Pad4();

	//	--- head ------------------------------------------------------------
	Writer Head;
	Head.U32(0x00010000);			// version
	Head.U32(0x00010000);			// fontRevision
	Head.U32(0);					// checkSumAdjustment (後で埋める)
	Head.U32(0x5F0F3CF5);			// magicNumber
	Head.U16(0x000B);				// flags
	Head.U16(UNITS_PER_EM);
	for (int i = 0; i < 16; i++) Head.U8(0);	// created / modified
	Head.I16(0); Head.I16(-DESCENT);			// xMin / yMin
	Head.I16(UNITS_PER_EM); Head.I16(ASCENT);	// xMax / yMax
	Head.U16(0);					// macStyle
	Head.U16(8);					// lowestRecPPEM
	Head.I16(2);					// fontDirectionHint
	Head.I16(1);					// indexToLocFormat (long)
	Head.I16(0);					// glyphDataFormat

	//	--- hhea / hmtx -----------------------------------------------------
	Writer Hhea;
	Hhea.U32(0x00010000);
	Hhea.I16(ASCENT); Hhea.I16(-DESCENT); Hhea.I16(0);
	Hhea.U16(UNITS_PER_EM);			// advanceWidthMax
	Hhea.I16(0); Hhea.I16(0); Hhea.I16(UNITS_PER_EM);
	Hhea.I16(1); Hhea.I16(0); Hhea.I16(0);
	for (int i = 0; i < 4; i++) Hhea.I16(0);	// reserved
	Hhea.I16(0);					// metricDataFormat
	Hhea.U16(NumGlyphs);			// numberOfHMetrics

	Writer Hmtx;
	for (int i = 0; i < NumGlyphs; i++) {
		Hmtx.U16(UNITS_PER_EM);		// advanceWidth
		Hmtx.I16(0);				// lsb
	}
	Hmtx.Pad4();

	//	--- maxp ------------------------------------------------------------
	//	**maxPoints / maxContours は実際の最大値を入れる事。**
	//	市松模様のような字形は 1 行あたりの矩形が増え、36x36 でも
	//	輪郭が 600 を超える。小さい値を書くとラスタライザが描画を諦める
	int MaxPoints = 0, MaxContours = 0;
	for (const TSMemoryDrcsGlyph &g : Glyphs) {
		const int n = static_cast<int>(ToRects(g).size());
		MaxContours = std::max(MaxContours, n);
		MaxPoints = std::max(MaxPoints, n * 4);
	}

	Writer Maxp;
	Maxp.U32(0x00010000);
	Maxp.U16(NumGlyphs);
	Maxp.U16(MaxPoints);
	Maxp.U16(MaxContours);
	Maxp.U16(0); Maxp.U16(0);
	Maxp.U16(2);					// maxZones
	Maxp.U16(0); Maxp.U16(0); Maxp.U16(0); Maxp.U16(0);
	Maxp.U16(0); Maxp.U16(0); Maxp.U16(0); Maxp.U16(0);
	Maxp.U16(0);

	//	--- OS/2 (version 4 / 96 バイト) ---------------------------------
	//	項目数を間違えると後続がずれる。仕様の順に 1 つずつ書く
	Writer Os2;
	Os2.U16(4);                     // version
	Os2.I16(UNITS_PER_EM);          // xAvgCharWidth
	Os2.U16(400);                   // usWeightClass
	Os2.U16(5);                     // usWidthClass
	Os2.U16(0);                     // fsType
	Os2.I16(0); Os2.I16(0); Os2.I16(0); Os2.I16(0);   // ySubscript X/Y Size,Offset
	Os2.I16(0); Os2.I16(0); Os2.I16(0); Os2.I16(0);   // ySuperscript X/Y Size,Offset
	Os2.I16(0); Os2.I16(0);         // yStrikeout Size / Position
	Os2.I16(0);                     // sFamilyClass
	for (int i = 0; i < 10; i++) Os2.U8(0);           // panose
	Os2.U32(0); Os2.U32(0); Os2.U32(0); Os2.U32(0);   // ulUnicodeRange1-4
	Os2.Raw("TSMM", 4);             // achVendID
	Os2.U16(0x0040);                // fsSelection (REGULAR)
	Os2.U16(Glyphs.front().Code);   // usFirstCharIndex
	Os2.U16(Glyphs.back().Code);    // usLastCharIndex
	Os2.I16(ASCENT);                // sTypoAscender
	Os2.I16(-DESCENT);              // sTypoDescender
	Os2.I16(0);                     // sTypoLineGap
	Os2.U16(ASCENT);                // usWinAscent
	Os2.U16(DESCENT);               // usWinDescent
	Os2.U32(0); Os2.U32(0);         // ulCodePageRange1-2
	Os2.I16(ASCENT / 2);            // sxHeight
	Os2.I16(ASCENT);                // sCapHeight
	Os2.U16(0);                     // usDefaultChar
	Os2.U16(0x20);                  // usBreakChar
	Os2.U16(1);                     // usMaxContext

	//	--- post ------------------------------------------------------------
	Writer Post;
	Post.U32(0x00030000);
	Post.U32(0);
	Post.I16(0); Post.I16(0);
	Post.U32(0);					// isFixedPitch
	Post.U32(0); Post.U32(0); Post.U32(0); Post.U32(0);

	//	--- name / cmap -----------------------------------------------------
	Writer Name;  BuildName(pszFamily, &Name);
	Writer Cmap;  BuildCmap(Glyphs, &Cmap);

	//	--- テーブルを並べる (タグの昇順) -----------------------------------
	struct Table { const char *Tag; Writer *W; };
	Table Tables[] = {
		{ "OS/2", &Os2 }, { "cmap", &Cmap }, { "glyf", &Glyf },
		{ "head", &Head }, { "hhea", &Hhea }, { "hmtx", &Hmtx },
		{ "loca", &LocaW }, { "maxp", &Maxp }, { "name", &Name },
		{ "post", &Post },
	};
	const int Count = static_cast<int>(sizeof(Tables) / sizeof(Tables[0]));

	int SearchRange = 16;
	int Entry = 0;
	while (SearchRange * 2 <= Count * 16) { SearchRange *= 2; Entry++; }

	Writer Out;
	Out.U32(0x00010000);
	Out.U16(Count);
	Out.U16(SearchRange);
	Out.U16(Entry);
	Out.U16(Count * 16 - SearchRange);

	uint32_t Offset = static_cast<uint32_t>(12 + 16 * Count);
	std::vector<uint32_t> Offsets;
	for (int i = 0; i < Count; i++) {
		Out.Raw(Tables[i].Tag, 4);
		Out.U32(CheckSum(Tables[i].W->Data.data(), Tables[i].W->Size()));
		Out.U32(Offset);
		Out.U32(static_cast<uint32_t>(Tables[i].W->Size()));
		Offsets.push_back(Offset);
		Offset += static_cast<uint32_t>((Tables[i].W->Size() + 3) & ~3u);
	}
	for (int i = 0; i < Count; i++) {
		while (Out.Size() < Offsets[i]) Out.U8(0);
		Out.Raw(Tables[i].W->Data.data(), Tables[i].W->Size());
		Out.Pad4();
	}

	//	head.checkSumAdjustment はフォント全体の合計から求める
	{
		const size_t HeadOffset = Offsets[3];			// 上の並びで head は 4 番目
		const uint32_t Total = CheckSum(Out.Data.data(), Out.Size());
		const uint32_t Adjust = 0xB1B0AFBAu - Total;
		BYTE *p = Out.Data.data() + HeadOffset + 8;
		p[0] = static_cast<BYTE>(Adjust >> 24);
		p[1] = static_cast<BYTE>(Adjust >> 16);
		p[2] = static_cast<BYTE>(Adjust >> 8);
		p[3] = static_cast<BYTE>(Adjust);
	}

	*pOut = std::move(Out.Data);
	return true;
}
