//----------------------------------------------------------------------------
//	字幕の取り出しと復号を実 TS で確かめる。
//
//	  test_caption <ts-file> [--dump]
//
//	組み立てた入力での単体検査 (TS 不要) と、実 TS での確認の 2 段構え。
//	合成した TS には字幕が入らない為、実データの癖 (集合の切り替え・
//	追加記号・外字) は実際の放送 TS でしか確かめられない。字幕を持たない
//	TS では skip する。
//----------------------------------------------------------------------------
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "arib_text.h"
#include "arib_to_aviutl2.h"
#include "arib_gaiji.h"

namespace {

int g_failures = 0;

//	第 3 引数で与える検索文字列 (省略時は nullptr)。
//	--dump の出力は先頭 12 件だけなので、実データの不具合を追う時に
//	見たい字幕を絞る為に使う
const wchar_t *g_pszFind = nullptr;

void check(const char *what, bool ok)
{
	std::printf("%-56s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		g_failures++;
}

const int TS_PACKET_SIZE = 188;

//---------------------------------------------------------------------------
//	PMT から字幕の PID を拾う
//---------------------------------------------------------------------------
struct CaptionPid {
	WORD Pid;
	bool fSuperimpose;		// component_tag 0x38- は文字スーパー
};

std::vector<CaptionPid> FindCaptionPids(const std::vector<BYTE> &Ts)
{
	std::vector<WORD> PmtPids;
	std::vector<CaptionPid> Out;

	for (size_t i = 0; i + TS_PACKET_SIZE <= Ts.size(); i += TS_PACKET_SIZE) {
		const BYTE *p = &Ts[i];
		if (p[0] != 0x47)
			continue;
		const WORD Pid = static_cast<WORD>(((p[1] & 0x1F) << 8) | p[2]);
		if ((p[1] & 0x40) == 0 || (p[3] & 0x10) == 0)
			continue;

		size_t o = 4;
		if (p[3] & 0x20)
			o += 1 + p[4];
		if (o >= TS_PACKET_SIZE)
			continue;
		o += 1 + p[o];							// pointer_field
		if (o + 12 > TS_PACKET_SIZE)
			continue;

		//	**セクションが 1 パケットに収まらない事がある。**
		//	丸ごと捨てず、収まっている範囲だけ読む (PID を拾えれば足りる)
		if (Pid == 0x0000 && p[o] == 0x00) {
			const size_t Len = ((p[o + 1] & 0x0F) << 8) | p[o + 2];
			size_t End = o + 3 + Len - 4;
			if (End > TS_PACKET_SIZE)
				End = TS_PACKET_SIZE;
			for (size_t q = o + 8; q + 4 <= End; q += 4) {
				if ((p[q] << 8 | p[q + 1]) != 0)
					PmtPids.push_back(static_cast<WORD>(((p[q + 2] & 0x1F) << 8) | p[q + 3]));
			}
			continue;
		}

		bool fIsPmt = false;
		for (WORD x : PmtPids)
			fIsPmt = fIsPmt || (x == Pid);
		if (!fIsPmt || p[o] != 0x02)
			continue;

		const size_t Len = ((p[o + 1] & 0x0F) << 8) | p[o + 2];
		size_t End = o + 3 + Len - 4;
		if (End > TS_PACKET_SIZE)
			End = TS_PACKET_SIZE;			// 収まっている範囲だけ読む
		const size_t InfoLen = ((p[o + 10] & 0x0F) << 8) | p[o + 11];
		size_t q = o + 12 + InfoLen;
		while (q + 5 <= End) {
			const BYTE Type = p[q];
			const WORD Es = static_cast<WORD>(((p[q + 1] & 0x1F) << 8) | p[q + 2]);
			const size_t DescLen = ((p[q + 3] & 0x0F) << 8) | p[q + 4];
			if (Type == 0x06) {
				bool fSuper = false;
				size_t d = q + 5;
				while (d + 2 <= q + 5 + DescLen && d + 2 <= End) {
					if (p[d] == 0x52 && p[d + 1] >= 1)		// stream_identifier
						fSuper = (p[d + 2] >= 0x38);
					d += 2 + p[d + 1];
				}
				bool fKnown = false;
				for (const CaptionPid &c : Out)
					fKnown = fKnown || (c.Pid == Es);
				if (!fKnown)
					Out.push_back({ Es, fSuper });
			}
			q += 5 + DescLen;
		}
		if (!Out.empty())
			break;
	}
	std::printf("  (PMT PIDs found: %zu)\n", PmtPids.size());
	return Out;
}

//---------------------------------------------------------------------------
//	字幕の PES から本文のデータユニットを取り出す
//---------------------------------------------------------------------------
struct CaptionUnit {
	int DataGroupId;
	std::vector<BYTE> Body;		// data_unit の中身
	BYTE Parameter;				// 0x20 = 本文 / 0x30,0x31 = DRCS
	//	PES の PTS (90kHz)。無ければ -1。
	//	**本文の無いユニットにも PTS が付いている。**
	//	そこが「消す時刻」なので、表示の長さを測るのに要る
	INT64 Pts = -1;
};

//	PES ヘッダから PTS を取り出す (90kHz)。無ければ -1
INT64 PesPts(const std::vector<BYTE> &Pes)
{
	if (Pes.size() < 14 || (Pes[7] & 0x80) == 0)
		return -1;
	const BYTE *p = &Pes[9];
	return (static_cast<INT64>(p[0] & 0x0E) << 29)
		 | (static_cast<INT64>(p[1]) << 22)
		 | (static_cast<INT64>(p[2] & 0xFE) << 14)
		 | (static_cast<INT64>(p[3]) << 7)
		 | (static_cast<INT64>(p[4]) >> 1);
}

void ParseCaptionPes(const std::vector<BYTE> &Pes, std::vector<CaptionUnit> *pOut)
{
	if (Pes.size() < 9 || Pes[0] != 0x00 || Pes[1] != 0x00 || Pes[2] != 0x01)
		return;
	const size_t HeaderLen = Pes[8];
	if (9 + HeaderLen + 3 > Pes.size())
		return;

	const BYTE *pl = &Pes[9 + HeaderLen];
	const size_t PlSize = Pes.size() - 9 - HeaderLen;
	if (PlSize < 3)
		return;

	const size_t Skip = 3 + (pl[2] & 0x0F);		// PES_data_packet_header
	if (Skip + 5 > PlSize)
		return;

	const BYTE *dg = pl + Skip;
	const size_t DgSize = PlSize - Skip;
	const int Id = (dg[0] >> 2) & 0x3F;
	const size_t Size = (dg[3] << 8) | dg[4];
	if (5 + Size > DgSize)
		return;

	const BYTE *body = dg + 5;
	size_t q = 1;
	const int Tmd = (body[0] >> 6) & 3;

	if (Id == 0x00 || Id == 0x20) {				// 字幕管理データ
		if (Tmd == 0x02)
			q += 5;
		if (q >= Size)
			return;
		const int Langs = body[q++];
		for (int i = 0; i < Langs && q < Size; i++) {
			const int Dmf = body[q++] & 0x0F;
			if ((Dmf >> 2) == 0x03)
				q++;
			q += 4;
		}
	} else {									// 字幕文データ
		if (Tmd == 0x01 || Tmd == 0x02)
			q += 5;
	}

	if (q + 3 > Size)
		return;
	const size_t Loop = (body[q] << 16) | (body[q + 1] << 8) | body[q + 2];
	q += 3;
	const size_t End = q + Loop < Size ? q + Loop : Size;

	while (q + 5 <= End) {
		if (body[q] != 0x1F)
			return;
		const BYTE Param = body[q + 1];
		const size_t Len = (body[q + 2] << 16) | (body[q + 3] << 8) | body[q + 4];
		if (q + 5 + Len > End)
			return;
		CaptionUnit u;
		u.DataGroupId = Id;
		u.Parameter = Param;
		u.Pts = PesPts(Pes);
		u.Body.assign(body + q + 5, body + q + 5 + Len);
		pOut->push_back(u);
		q += 5 + Len;
	}
}

}	// namespace


namespace {

//	実データから写した並びで復号器を検査する。
//	**TS が無くても走る**ので、ここが復号器の常設の検査になる。
void RunUnitTests()
{
	std::printf("=== 8 単位符号の復号 (組み立てた入力) ===\n");

	//	1. 漢字・ひらがな・カタカナ。GL は初期状態で漢字 (2 バイト)
	{
		//	（コタロー）は今年
		const BYTE d[] = {
			0x21, 0x4A, 0x25, 0x33, 0x25, 0x3F, 0x25, 0x6D,
			0x21, 0x3C, 0x21, 0x4B,
			0x24, 0x4F, 0x3A, 0x23, 0x47, 0x2F,
		};
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		const std::wstring s = AribItemsToPlainText(Items);
		check("kanji / katakana / hiragana decode",
			  s == L"（コタロー）は今年");
	}

	//	2. 色 (BKF..WHF) と大きさ (SSZ/MSZ/NSZ)
	{
		const BYTE d[] = { 0x86, 0x88, 0x24, 0x22, 0x8A, 0x24, 0x24 };
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		int Colors = 0, Sizes = 0;
		for (const AribItem &it : Items) {
			if (it.Type == AribItemType::Color) Colors++;
			if (it.Type == AribItemType::Size) Sizes++;
		}
		check("colour and size controls are recognised", Colors == 1 && Sizes == 2);
		check("text around the controls survives",
			  AribItemsToPlainText(Items) == L"あい");
	}

	//	3. **0x90 の引数の数**。実データでは `90 20 44` と `90 51` の
	//	   両方が現れる。1 バイト固定で読むと以降が全てずれる
	{
		const BYTE d[] = { 0x90, 0x20, 0x44, 0x90, 0x51, 0x24, 0x22 };
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		check("0x90 with 0x20 takes two bytes, otherwise one",
			  AribItemsToPlainText(Items) == L"あ");

		//	**`90 20 44` は色そのものではなく色配列 (CLUT) の選択。**
		//	これを前景色として扱うと字幕がほぼ全て同じ色に染まる
		//	(実測では全ての字幕文が青字になっていた)。
		//	`90 51` の背景色は選ばれた配列の中の 1 番なので 4 * 16 + 1
		int Colors = 0, Backs = -1;
		for (const AribItem &it : Items) {
			if (it.Type == AribItemType::Color) Colors++;
			if (it.Type == AribItemType::BackColor) Backs = it.A;
		}
		check("the colour map selection is not a foreground colour",
			  Colors == 0);
		check("the colour index is taken within the selected map",
			  Backs == 4 * 16 + 1);
		//	**索引 65 は半透明の黒。**これが字幕の背景の正体で、
		//	以前は「判らない色」として捨てていた
		check("the background colour is the translucent black",
			  AribColorIsKnown(Backs) && AribColorToRgb(Backs) == 0x000000
			  && AribColorAlpha(Backs) == 128);

		//	背景の既定 (8 番) は透明。黒として出すと何も無い所に黒が付く
		check("the default background is transparent",
			  !AribColorIsKnown(8) && AribColorAlpha(8) == 0);
		check("an opaque colour is reported as known",
			  AribColorIsKnown(7) && AribColorAlpha(7) == 255);
	}

	//	4. CSI (可変長)。位置指定等がテキストを飲み込まない事
	{
		//	CSI "36;36" 0x20 'W' の後に「あ」
		const BYTE d[] = {
			0x9B, 0x33, 0x36, 0x3B, 0x33, 0x36, 0x20, 0x57, 0x24, 0x22,
		};
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		check("a CSI sequence is skipped whole",
			  AribItemsToPlainText(Items) == L"あ");
	}

	//	5. APS (位置指定)
	{
		const BYTE d[] = { 0x1C, 0x45, 0x4A, 0x24, 0x22 };
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		bool fPos = false;
		for (const AribItem &it : Items)
			fPos = fPos || (it.Type == AribItemType::Position);
		check("APS is reported as a position", fPos);
		check("text after APS is kept", AribItemsToPlainText(Items) == L"あ");
	}

	//	5b. **ACPS (CSI ... 0x20 'a')**。放送はこれで 1 行ずつ位置を打つ。
	//	   CSI を丸ごと読み飛ばすと行が全て繋がって 1 行になる
	{
		//	CSI "36;36" SP 'W' (文字 36x36) / CSI "24" SP 'Y' (行間 24) /
		//	CSI "200;449" SP 'a' の後に「あ」
		const BYTE d[] = {
			0x9B, 0x33, 0x36, 0x3B, 0x33, 0x36, 0x20, 0x57,
			0x9B, 0x32, 0x34, 0x20, 0x59,
			0x9B, 0x32, 0x30, 0x30, 0x3B, 0x34, 0x34, 0x39, 0x20, 0x61,
			0x24, 0x22,
		};
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		int X = -1, Y = -1, Pitch = -1;
		for (const AribItem &it : Items) {
			if (it.Type == AribItemType::Position) {
				X = it.A; Y = it.B; Pitch = it.C;
			}
		}
		check("ACPS gives the position in dots", X == 200 && Y == 449);
		check("the line pitch comes from SSM and SVS", Pitch == 36 + 24);
		check("text after ACPS is kept", AribItemsToPlainText(Items) == L"あ");
	}

	//	5b2. **画面消去 (CS = 0x0C)。**放送はこれで「ここで消す」と送る。
	//	   拾えないと次の字幕が来るまで出しっぱなしになる
	//	   (実測: 17 件中 1 件、最大 2.4 秒長く出ていた)
	{
		const BYTE d[] = { 0x0C };
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		bool fClear = false;
		for (const AribItem &it : Items)
			fClear = fClear || (it.Type == AribItemType::ClearScreen);
		check("CS is reported as a clear-screen", fClear);
		//	本文が無いので行にはならない。**この組み合わせが「消す」の印**
		AribToAviUtl2Options o2;
		std::vector<int> Drcs;
		AribCaptionLayout L;
		AribItemsToAviUtl2(Items, o2, &Drcs, &L);
		check("a clear-only unit produces no line", L.Lines.empty());
	}

	//	5b3. **SDF (CSI ... 0x20 'V') = 表示領域。右端で自動的に折り返す。**
	//	   放送は 1 行に収まらない字幕をこれで 2 行にしている。
	//	   読まないと 1 行に繋がったまま画面をはみ出す
	//	   (実測: 平面 960 幅に対して右端が 1230 になっていた)
	{
		//	SDP 170;30 / SDF 200;480 (右端 = 170+200 = 370) /
		//	SSM 36;36 / SHS 4 (送り 40) / ACPS 170;449 のあと「あいうえお」
		const BYTE d[] = {
			0x9B, 0x31, 0x37, 0x30, 0x3B, 0x33, 0x30, 0x20, 0x5F,
			0x9B, 0x32, 0x30, 0x30, 0x3B, 0x34, 0x38, 0x30, 0x20, 0x56,
			0x9B, 0x33, 0x36, 0x3B, 0x33, 0x36, 0x20, 0x57,
			0x9B, 0x34, 0x20, 0x58,
			0x9B, 0x32, 0x34, 0x20, 0x59,
			0x9B, 0x31, 0x37, 0x30, 0x3B, 0x34, 0x34, 0x39, 0x20, 0x61,
			0x24, 0x22, 0x24, 0x24, 0x24, 0x26, 0x24, 0x28, 0x24, 0x2A,
		};
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);

		//	170,210,250,290,330 の 5 文字目までは収まる (330+40 = 370)。
		//	6 文字目は 370+40 > 370 なので折り返す
		int Positions = 0, LastY = -1;
		for (const AribItem &it : Items) {
			if (it.Type == AribItemType::Position) {
				Positions++;
				LastY = it.B;
			}
		}
		check("SDF makes the pen wrap at the right edge",
			  Positions == 1 && AribItemsToPlainText(Items) == L"あいうえお");

		//	6 文字目を足すと折り返しの位置指定が増える
		std::vector<BYTE> e(d, d + sizeof(d));
		e.push_back(0x24); e.push_back(0x2B);		// か
		Items.clear();
		AribDecodeText(e.data(), e.size(), &Items);
		Positions = 0; LastY = -1;
		int WrapX = -1;
		for (const AribItem &it : Items) {
			if (it.Type == AribItemType::Position) {
				Positions++;
				WrapX = it.A;
				LastY = it.B;
			}
		}
		//	折り返し先は表示領域の左端、1 行 (36+24=60) 下
		check("the wrapped line starts at the left edge one row below",
			  Positions == 2 && WrapX == 170 && LastY == 449 + 60);
	}

	//	5b4. **外字の指定は別の表。**ESC ... 0x20 F の F は外字の番号で、
	//	   本文の集合とは意味が違う。混ぜると外字 2 番 (0x42) が漢字集合に
	//	   なり、以降の本文が 2 バイトで読まれて丸ごと化ける
	{
		//	ESC 0x28 0x20 0x42 = G0 に外字 2 番 (1 バイト) / そのあと 0x21
		const BYTE d[] = { 0x1B, 0x28, 0x20, 0x42, 0x21 };
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		bool fDrcs = false;
		for (const AribItem &it : Items)
			fDrcs = fDrcs || (it.Type == AribItemType::Drcs);
		check("a DRCS designation is not read as a character set", fDrcs);
	}

	//	5b5. **プロポーショナル集合。**字形の表は普通の集合と同じ。
	//	   対応していないとその集合の文字が丸ごと消える
	{
		//	ESC 0x28 0x37 = G0 にプロポーショナルひらがな / 0x22 = あ
		const BYTE d[] = { 0x1B, 0x28, 0x37, 0x22 };
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		check("the proportional hiragana set is decoded",
			  AribItemsToPlainText(Items) == L"あ");
	}

	//	5b6. **ペンを動かす符号 (APF / APB / APU / PAPF)。**
	//	   実測の 33 番組では来ないが、来た時に送りへ反映しないと
	//	   以降の位置がずれる
	{
		//	ACPS 170;449 のあと APF (1 つ進む) / PAPF 2 (2 つ進む) / 「あ」
		const BYTE d[] = {
			0x9B, 0x33, 0x36, 0x3B, 0x33, 0x36, 0x20, 0x57,
			0x9B, 0x34, 0x20, 0x58,
			0x9B, 0x31, 0x37, 0x30, 0x3B, 0x34, 0x34, 0x39, 0x20, 0x61,
			0x09,					// APF
			0x16, 0x42,				// PAPF 2 (0x42 & 0x3F = 2)
			0x24, 0x22,				// あ
		};
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		int Left = -1;
		for (const AribItem &it : Items) {
			if (it.Type == AribItemType::Text)
				Left = it.D;
		}
		//	170 + (1 + 2) * 40 = 290
		check("APF and PAPF move the pen", Left == 290);
	}

	//	5b7. **RPC (0x98) = 次の文字を繰り返す。**回数は P1 - 0x40。
	//	   無視すると 1 文字しか出ず、以降の位置も繰り返した分ずれる。
	//	   caption.dll (TVCaptionMod2 が使っている物) と同じ解釈
	{
		//	SSM 36;36 / SHS 4 (送り 40) / ACPS 170;449 / RPC 3 / 「あ」
		const BYTE d[] = {
			0x9B, 0x33, 0x36, 0x3B, 0x33, 0x36, 0x20, 0x57,
			0x9B, 0x34, 0x20, 0x58,
			0x9B, 0x31, 0x37, 0x30, 0x3B, 0x34, 0x34, 0x39, 0x20, 0x61,
			0x98, 0x43,				// RPC 3 (0x43 - 0x40)
			0x24, 0x22,				// あ
			0x24, 0x24,				// い (繰り返しは使い切られている)
		};
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		check("RPC repeats the next character",
			  AribItemsToPlainText(Items) == L"あああい");

		//	送りも繰り返した分だけ進む。170 + 4 * 40 = 330
		int Last = -1;
		for (const AribItem &it : Items) {
			if (it.Type == AribItemType::Text)
				Last = it.C;
		}
		check("RPC advances the pen for every copy", Last == 330);
	}

	//	5b8. **RPC 0 = 行末まで繰り返す。**
	{
		//	SDP 170;30 / SDF 200;480 (右端 370) なので 5 文字分入る
		const BYTE d[] = {
			0x9B, 0x31, 0x37, 0x30, 0x3B, 0x33, 0x30, 0x20, 0x5F,
			0x9B, 0x32, 0x30, 0x30, 0x3B, 0x34, 0x38, 0x30, 0x20, 0x56,
			0x9B, 0x33, 0x36, 0x3B, 0x33, 0x36, 0x20, 0x57,
			0x9B, 0x34, 0x20, 0x58,
			0x9B, 0x31, 0x37, 0x30, 0x3B, 0x34, 0x34, 0x39, 0x20, 0x61,
			0x98, 0x40,				// RPC 0 = 行末まで
			0x24, 0x22,				// あ
		};
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		check("RPC 0 fills to the end of the line",
			  AribItemsToPlainText(Items) == L"あああああ");
	}

	//	5b9. **PLD / PLU = 半行下 / 半行上。**上付き・下付きに使われる。
	//	   **半行なので行の区切りにはならない**
	//	   (変換側の閾値は行送りの 3/4)
	{
		//	ACPS 170;449 / PLD / 「あ」
		const BYTE d[] = {
			0x9B, 0x33, 0x36, 0x3B, 0x33, 0x36, 0x20, 0x57,
			0x9B, 0x32, 0x34, 0x20, 0x59,
			0x9B, 0x31, 0x37, 0x30, 0x3B, 0x34, 0x34, 0x39, 0x20, 0x61,
			0x9B, 0x20, 0x5B,		// PLD
			0x24, 0x22,
		};
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		int LastY = -1;
		for (const AribItem &it : Items) {
			if (it.Type == AribItemType::Position)
				LastY = it.B;
		}
		//	行送り 60 の半分だけ下がる
		check("PLD moves the pen down half a row", LastY == 449 + 30);

		AribToAviUtl2Options o2;
		o2.UseBroadcastColor = false;
		std::vector<int> Drcs;
		AribCaptionLayout L;
		AribItemsToAviUtl2(Items, o2, &Drcs, &L);
		check("a half-row move does not split the line", L.Lines.size() == 1);
	}

	//	5b10. **MACRO (0x95) は 0x4F まで抱えている。**
	//	   引数 1 つとして食べると**定義の中身が本文に混ざる**。
	//	   caption.dll も 0x4F まで読み飛ばしている
	{
		//	MACRO の定義 (中身は「あ」に見えるバイト) のあとに「い」
		const BYTE d[] = {
			0x95, 0x40, 0x24, 0x22, 0x4F,	// マクロ定義 (0x4F で終わる)
			0x24, 0x24,						// い
		};
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		check("a macro definition is skipped up to 0x4F",
			  AribItemsToPlainText(Items) == L"い");
	}

	//	5b11. **0x4F が見つからない時は 1 バイトだけ食べる。**
	//	   壊れたデータで残り全部を飲み込まない為
	{
		const BYTE d[] = { 0x95, 0x40, 0x24, 0x22 };
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		check("a broken macro does not swallow the rest",
			  AribItemsToPlainText(Items) == L"あ");
	}

	//	5b12. **行送りは「文字を書く時」の大きさで決まる。**
	//	   放送は位置を打ってから大きさを指定して来る (実測):
	//	     CSI "250;389" SP 'a' / 88 (小型) / ゆうた
	//	     CSI "170;449" SP 'a' / 8A (標準) / 乙骨憂太
	//	   位置指定の時点の大きさで決めると、1 行目が小型の送り 30 で
	//	   計算され、**次の行と半行ぶん重なる** (実機で発生)
	{
		const BYTE d[] = {
			0x9B, 0x33, 0x36, 0x3B, 0x33, 0x36, 0x20, 0x57,	// SSM 36;36
			0x9B, 0x32, 0x34, 0x20, 0x59,					// SVS 24
			0x9B, 0x31, 0x37, 0x30, 0x3B, 0x34, 0x34, 0x39, 0x20, 0x61,
			0x88,											// SSZ (位置の後)
			0x24, 0x22,										// あ
		};
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		int Pitch = -1;
		for (const AribItem &it : Items) {
			if (it.Type == AribItemType::Position)
				Pitch = it.C;
		}
		//	小型なので (36+24)/2 = 30 に入れ直される
		check("a size after the position retunes the row pitch", Pitch == 30);

		//	標準に戻したら 60 に戻る
		std::vector<BYTE> e(d, d + sizeof(d) - 2);
		e.push_back(0x8A);								// NSZ
		e.push_back(0x24); e.push_back(0x22);
		Items.clear();
		AribDecodeText(e.data(), e.size(), &Items);
		Pitch = -1;
		for (const AribItem &it : Items) {
			if (it.Type == AribItemType::Position)
				Pitch = it.C;
		}
		check("the pitch follows the last size before the text", Pitch == 60);
	}

	//	5c. **ORN (CSI ... 0x20 'c') = 文字外縁 (縁取り)。**
	//	   放送が実際に送って来る (実測: 8 本中 2 本で 12 件、全て黒)。
	//	   **色は 1 つの数に詰められている。**P2 = 色配列 * 100 + 色番号 で、
	//	   CLUT の索引は 色配列 * 16 + 色番号。100 で割る所を取り違えると
	//	   関係の無い色になる
	{
		//	CSI "1;0000" SP 'c' … 縁取りあり、色配列 0 の 0 番 (黒)
		const BYTE d[] = {
			0x9B, 0x31, 0x3B, 0x30, 0x30, 0x30, 0x30, 0x20, 0x63,
			0x24, 0x22,
		};
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		int Orn = -1, Color = -1;
		for (const AribItem &it : Items) {
			if (it.Type == AribItemType::Ornament) { Orn = it.A; Color = it.B; }
		}
		check("ORN reports the outline and its colour", Orn == 1 && Color == 0);
		check("CLUT 0 is opaque black",
			  TSMemoryAribClut(0).R == 0 && TSMemoryAribClut(0).A == 255);
		check("text after ORN is kept", AribItemsToPlainText(Items) == L"あ");
	}

	//	5d. 色配列を伴う ORN。CSI "1;0305" SP 'c' = 色配列 3 の 5 番
	{
		const BYTE d[] = {
			0x9B, 0x31, 0x3B, 0x30, 0x33, 0x30, 0x35, 0x20, 0x63,
		};
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		int Color = -1;
		for (const AribItem &it : Items) {
			if (it.Type == AribItemType::Ornament) Color = it.B;
		}
		check("the ORN colour is colour-map * 16 + number",
			  Color == 3 * 16 + 5);
	}

	//	5e. **APS の行送りは文字サイズで変わる**。
	//	   小型 (SSZ) の行に標準の送りを使うと画面の外を指す
	//	   (実測: 区切りの行が y=990 になり 540 の字幕平面をはみ出した)
	{
		//	SSZ の後に APS 行 15 / 桁 0。行送りは (36+24)/2 = 30
		const BYTE d[] = { 0x88, 0x1C, 0x4F, 0x40, 0x24, 0x22 };
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		int Y = -1;
		for (const AribItem &it : Items) {
			if (it.Type == AribItemType::Position)
				Y = it.B;
		}
		check("APS uses half the line pitch under SSZ", Y == 16 * 30);
	}

	//	5f. 標準の大きさなら送りはそのまま
	{
		const BYTE d[] = { 0x8A, 0x1C, 0x47, 0x40, 0x24, 0x22 };
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		int Y = -1;
		for (const AribItem &it : Items) {
			if (it.Type == AribItemType::Position)
				Y = it.B;
		}
		check("APS uses the full line pitch under NSZ", Y == 8 * 60);
	}

	//	5c. **追加記号の対応表**。CP932 経由にすると別の文字になる。
	//	   区 92 点 92 は CP932 だと「釗」になっていた (実測)
	{
		struct { int Ku, Ten; const wchar_t *Expect; } T[] = {
			{  1, 33, L"〜" },		// 〜 (CP932 だと ～ になる)
			{  5, 65, L"メ" },		// メ
			{ 90, 53, nullptr },		// 🈐 (BMP 外。長さで見る)
			{ 93, 90, L"♬" },		// ♬ (CP932 に無く、外字扱いだった)
			{ 92, 92, nullptr },		// 未定義
		};
		bool fOk = true;
		int Len = 0;
		for (const auto &t : T) {
			const WCHAR *p = TSMemoryAribKuTen(t.Ku, t.Ten, &Len);
			if (t.Expect != nullptr)
				fOk = fOk && p != nullptr && Len == 1 && p[0] == t.Expect[0];
		}
		check("the ARIB table gives the broadcast characters", fOk);

		TSMemoryAribKuTen(92, 92, &Len);
		check("an undefined ku/ten is reported as missing",
			  TSMemoryAribKuTen(92, 92, &Len) == nullptr && Len == 0);

		//	BMP の外にある記号はサロゲートペアの 2 個で返る
		TSMemoryAribKuTen(90, 53, &Len);
		check("a symbol outside the BMP comes back as a surrogate pair",
			  TSMemoryAribKuTen(90, 53, &Len) != nullptr && Len == 2);
	}

	//	5d. 追加記号が外字ではなく本文として出る事。
	//	   ESC 0x24 0x2A 0x3B で G2 に追加記号を割り当て、SS2 で 1 文字呼ぶ
	{
		//	区 93 点 90 = ♬。以前は CP932 に無く外字扱いになっていた
		const BYTE d[] = { 0x1B, 0x24, 0x28, 0x3B, 0x7D, 0x7A };
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		check("an additional symbol decodes into text, not a DRCS",
			  AribItemsToPlainText(Items) == L"♬");
	}

	//	6. 外字 (DRCS)。ESC 0x24 0x28 0x20 0x41 で G0 を 2 バイト外字にする
	{
		const BYTE d[] = { 0x1B, 0x24, 0x28, 0x20, 0x41, 0x21, 0x21 };
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		bool fDrcs = false;
		for (const AribItem &it : Items)
			fDrcs = fDrcs || (it.Type == AribItemType::Drcs);
		check("a DRCS reference is reported", fDrcs);
	}

	//	7. 英数 (GL を G1 に切り替える LS1)。
	//	   **英数は全角。**中型 (MSZ) の時に半角相当の見た目になるので、
	//	   半角で出すと <tw50> と重なって細くなり過ぎる
	{
		const BYTE d[] = { 0x0E, 0x41, 0x42, 0x43 };
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		check("LS1 switches to the alphanumeric set",
			  AribItemsToPlainText(Items) == L"ＡＢＣ");
	}

	//	7b. **1 バイトの文字集合は区 4 / 区 5 では引けない。**
	//	   末尾に「ー」等が入っており、区で引くと落ちる
	//	   (実測: ステーション -> ステション)
	{
		//	LS1R で GR を G1 (カタカナ) にしてから 0xB9 0xC6 0xF9
		//	= ス テ ー … ではなく、マクロ後の並びに合わせて直接引く
		check("the katakana set has the prolonged sound mark",
			  TSMemoryAribKatakana(0x79) == L'ー');
		check("the hiragana set has the ideographic comma",
			  TSMemoryAribHiragana(0x7D) == L'、');
		//	**全角の片仮名は半角の表に入れてはいけない。**
		//	入れると片仮名集合 (ESC 0x31) から来た「ア」まで
		//	中型で半角になってしまう。半角に写すのは
		//	JIS X 0201 片仮名の集合だけ
		check("the katakana set is not in the halfwidth map",
			  TSMemoryAribHalfwidth(L'ア') == 0);
		check("the JIS X 0201 katakana set has its own halfwidth form",
			  TSMemoryAribJisKatakanaHalf(0x31) == 0xFF71		// ｱ
			  && TSMemoryAribJisKatakana(0x31) == L'ア');
		check("the halfwidth map covers the ideographic full stop",
			  TSMemoryAribHalfwidth(L'。') == 0xFF61);
		check("a character with no halfwidth form is not mapped",
			  TSMemoryAribHalfwidth(L'あ') == 0);
		check("halfwidth detection matches libaribcaption",
			  TSMemoryAribIsHalfwidth(0xFF61)
			  && TSMemoryAribIsHalfwidth(L' ')
			  && !TSMemoryAribIsHalfwidth(L'。')
			  && !TSMemoryAribIsHalfwidth(0));
	}

	//	8. 壊れた入力で落ちない事 (切り詰め・不正な区点)
	{
		const BYTE d[] = { 0x1B, 0x24 };			// ESC の途中で終わる
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		const BYTE e[] = { 0x7E, 0x7E, 0x9B };		// 不正な区点 + CSI の途中
		AribDecodeText(e, sizeof(e), &Items);
		check("a truncated or invalid stream does not crash", true);
	}

	std::printf("\n");
}

//	AviUtl2 のテキストへの変換
void RunConvertTests()
{
	std::printf("=== AviUtl2 のテキストへの変換 ===\n");

	AribToAviUtl2Options opt;
	opt.Preset = L"字幕";
	opt.DrcsFont = L"TSMemoryDRCS";

	//	1. プリセットが先頭に入る (書体を一括で変える為の要)
	{
		std::vector<AribItem> Items;
		AribItem t; t.Type = AribItemType::Text; t.Text = L"あ";
		Items.push_back(t);
		std::vector<int> Drcs;
		check("the preset is emitted first",
			  AribItemsToAviUtl2(Items, opt, &Drcs) == L"<$字幕>あ");
	}

	//	2. 色。同じ色が続く時は繰り返さない
	{
		std::vector<AribItem> Items;
		AribItem c; c.Type = AribItemType::Color; c.A = 7;	// 白
		AribItem t; t.Type = AribItemType::Text; t.Text = L"あ";
		Items.push_back(c); Items.push_back(t);
		Items.push_back(c); Items.push_back(t);			// 同じ色をもう一度
		std::vector<int> Drcs;
		check("colour becomes a control character and is not repeated",
			  AribItemsToAviUtl2(Items, opt, &Drcs) == L"<$字幕><#ffffff>ああ");
	}

	//	3. 放送の色を使わない設定
	{
		std::vector<AribItem> Items;
		AribItem c; c.Type = AribItemType::Color; c.A = 1;
		AribItem t; t.Type = AribItemType::Text; t.Text = L"あ";
		Items.push_back(c); Items.push_back(t);
		AribToAviUtl2Options o2 = opt;
		o2.UseBroadcastColor = false;
		std::vector<int> Drcs;
		check("the broadcast colour can be turned off",
			  AribItemsToAviUtl2(Items, o2, &Drcs) == L"<$字幕>あ");
	}

	//	3b. 背景色。<#文字色,影縁色> の 2 つ目に流す。
	//	**AviUtl2 には「背景の箱」が無い。**放送の黒背景は影・縁色として
	//	渡し、テキストプリセット側の文字装飾 (縁取り) で見える形にする
	{
		std::vector<AribItem> Items;
		AribItem c; c.Type = AribItemType::Color; c.A = 7;		// 白
		AribItem b; b.Type = AribItemType::BackColor; b.A = 0;	// 黒
		AribItem t; t.Type = AribItemType::Text; t.Text = L"あ";
		Items.push_back(c); Items.push_back(b); Items.push_back(t);
		std::vector<int> Drcs;
		check("the background colour becomes the second colour",
			  AribItemsToAviUtl2(Items, opt, &Drcs) == L"<$字幕><#ffffff,000000>あ");
	}

	//	3c. 背景色だけを切れる
	{
		std::vector<AribItem> Items;
		AribItem c; c.Type = AribItemType::Color; c.A = 7;
		AribItem b; b.Type = AribItemType::BackColor; b.A = 0;
		AribItem t; t.Type = AribItemType::Text; t.Text = L"あ";
		Items.push_back(c); Items.push_back(b); Items.push_back(t);
		AribToAviUtl2Options o2 = opt;
		o2.UseBroadcastBackColor = false;
		std::vector<int> Drcs;
		check("the background colour can be turned off",
			  AribItemsToAviUtl2(Items, o2, &Drcs) == L"<$字幕><#ffffff>あ");
	}

	//	3l. **中型 (MSZ) は <tw0.5>。**<tw> は百分率ではなく倍率で、
	//	   <tw50> と書くと 50 倍に引き伸ばされ、文字が横一線に潰れて
	//	   画面を横切る (実測で発生)
	{
		std::vector<AribItem> Items;
		AribItem m; m.Type = AribItemType::Size; m.A = 5; m.B = 10;	// 中型
		AribItem t; t.Type = AribItemType::Text; t.Text = L"あ";
		AribItem n; n.Type = AribItemType::Size; n.A = 10; n.B = 10;	// 標準
		AribItem t2; t2.Type = AribItemType::Text; t2.Text = L"い";
		Items.push_back(m); Items.push_back(t);
		Items.push_back(n); Items.push_back(t2);
		AribToAviUtl2Options o2 = opt;
		o2.UseBroadcastColor = false;
		std::vector<int> Drcs;
		check("the middle size is a scale, not a percentage",
			  AribItemsToAviUtl2(Items, o2, &Drcs) == L"<$字幕><tw0.5>あ<tw>い");
	}

	//	3m. **中型 (MSZ) は「横に潰す」ではなく「半角形を使う」指定。**
	//	   `。` を <tw0.5> で潰すと丸が楕円になる (実機で発生。
	//	   TVCaptionMod2 は半角の `｡` を等倍で描いていた)。
	//	   libaribcaption も横倍率が縦の半分の時に半角の表へ差し替え、
	//	   描画側は半角になった字に横倍率を掛けない
	{
		std::vector<AribItem> Items;
		AribItem m; m.Type = AribItemType::Size; m.A = 5; m.B = 10;	// 中型
		AribItem t; t.Type = AribItemType::Text; t.Text = L"。";
		Items.push_back(m); Items.push_back(t);
		AribToAviUtl2Options o2 = opt;
		o2.UseBroadcastColor = false;
		std::vector<int> Drcs;
		check("MSZ uses the halfwidth form instead of squashing",
			  AribItemsToAviUtl2(Items, o2, &Drcs) == L"<$字幕>｡");
	}

	//	3n. 半角形の無い字は従来通り横半分に潰す。
	//	   1 つの並びに両方が混ざったら、そこで区切って出し分ける
	{
		std::vector<AribItem> Items;
		AribItem m; m.Type = AribItemType::Size; m.A = 5; m.B = 10;
		AribItem t; t.Type = AribItemType::Text; t.Text = L"あ。あ";
		Items.push_back(m); Items.push_back(t);
		AribToAviUtl2Options o2 = opt;
		o2.UseBroadcastColor = false;
		std::vector<int> Drcs;
		check("a run is split where the halfwidth form runs out",
			  AribItemsToAviUtl2(Items, o2, &Drcs)
			  == L"<$字幕><tw0.5>あ<tw>｡<tw0.5>あ");
	}

	//	3o. **半角化するのは中型の時だけ。**標準 (NSZ) では全角のまま
	{
		std::vector<AribItem> Items;
		AribItem t; t.Type = AribItemType::Text; t.Text = L"。Ａ";
		Items.push_back(t);
		AribToAviUtl2Options o2 = opt;
		o2.UseBroadcastColor = false;
		std::vector<int> Drcs;
		check("the normal size keeps the fullwidth form",
			  AribItemsToAviUtl2(Items, o2, &Drcs) == L"<$字幕>。Ａ");
	}

	//	3p. 全角英数と全角の空白も中型では半角になる
	{
		std::vector<AribItem> Items;
		AribItem m; m.Type = AribItemType::Size; m.A = 5; m.B = 10;
		AribItem t; t.Type = AribItemType::Text; t.Text = L"Ａ　Ｂ";
		Items.push_back(m); Items.push_back(t);
		AribToAviUtl2Options o2 = opt;
		o2.UseBroadcastColor = false;
		std::vector<int> Drcs;
		check("MSZ turns fullwidth ASCII and space into halfwidth",
			  AribItemsToAviUtl2(Items, o2, &Drcs) == L"<$字幕>A B");
	}

	//	3q. **ルビ。**小型 (SSZ) で本文の 1 行上に、まとまりごとに
	//	   位置を打って書かれる。X の重なりで本文のどの字に掛かるかを決める。
	//	   実測の並び (『呪術廻戦』第 3 期):
	//	     SSZ / ACPS 230,449 / あいぞう / ACPS 390,449 / うずま
	//	     ACPS 170,509 / NSZ / ♬ 愛憎愛憎渦巻いて
	{
		std::vector<AribItem> Items;
		AribItem g; g.Type = AribItemType::Geometry; g.A = 36; g.B = 540;
		Items.push_back(g);

		//	ルビ (小型)
		AribItem ssz; ssz.Type = AribItemType::Size; ssz.A = 5; ssz.B = 5;
		Items.push_back(ssz);
		AribItem rp; rp.Type = AribItemType::Position;
		rp.A = 230; rp.B = 449; rp.C = 30; rp.D = 20;
		Items.push_back(rp);
		const wchar_t *Ruby = L"あいぞう";
		for (int n = 0; n < 4; n++) {
			AribItem t; t.Type = AribItemType::Text;
			t.Text = std::wstring(1, Ruby[n]);
			t.D = 230 + n * 20; t.C = t.D + 20;
			Items.push_back(t);
		}

		//	本文 (標準)。1 行下
		AribItem bp; bp.Type = AribItemType::Position;
		bp.A = 230; bp.B = 509; bp.C = 60; bp.D = 40;
		Items.push_back(bp);
		AribItem nsz; nsz.Type = AribItemType::Size; nsz.A = 10; nsz.B = 10;
		Items.push_back(nsz);
		const wchar_t *Body = L"愛憎渦巻";
		for (int n = 0; n < 4; n++) {
			AribItem t; t.Type = AribItemType::Text;
			t.Text = std::wstring(1, Body[n]);
			t.D = 230 + n * 40; t.C = t.D + 40;
			Items.push_back(t);
		}

		AribToAviUtl2Options o2 = opt;
		o2.UseBroadcastColor = false;
		std::vector<int> Drcs;
		check("ruby becomes a furigana block",
			  AribItemsToAviUtl2(Items, o2, &Drcs)
			  == L"<$字幕></>愛憎<!>あいぞう</>渦巻");

		//	**切ると従来どおり本文に混ぜる。**aviutl2.txt に
		//	「制御文字との組み合わせによっては正しく描画出来ない」とある為
		o2.UseRuby = false;
		Drcs.clear();
		check("ruby can be turned off",
			  AribItemsToAviUtl2(Items, o2, &Drcs)
			  == L"<$字幕><s><s*0.5>あいぞう<s>愛憎渦巻");
	}

	//	3q2. **2 行の字幕で、2 行目のルビが 1 行目に乗らない事。**
	//	   放送は
	//	     1 行目の位置 / 1 行目の本文
	//	     ルビの位置 (1 行目と 2 行目の間) / ルビ
	//	     2 行目の位置 / 2 行目の本文
	//	   の順で送る。受け取った時点の行に付けると 1 行目に乗る (実機で発生)
	{
		std::vector<AribItem> Items;
		AribItem nsz; nsz.Type = AribItemType::Size; nsz.A = 10; nsz.B = 10;
		AribItem ssz; ssz.Type = AribItemType::Size; ssz.A = 5; ssz.B = 5;

		//	1 行目 (y=449)
		AribItem p1; p1.Type = AribItemType::Position;
		p1.A = 200; p1.B = 449; p1.C = 60; p1.D = 40;
		Items.push_back(nsz); Items.push_back(p1);
		AribItem a; a.Type = AribItemType::Text; a.Text = L"上";
		a.D = 200; a.C = 240;
		Items.push_back(a);

		//	ルビ (1 行目と 2 行目の間、y=479)。**2 行目の「下」に掛かる**
		AribItem rp; rp.Type = AribItemType::Position;
		rp.A = 200; rp.B = 479; rp.C = 30; rp.D = 20;
		Items.push_back(ssz); Items.push_back(rp);
		AribItem r; r.Type = AribItemType::Text; r.Text = L"し";
		r.D = 200; r.C = 220;
		Items.push_back(r);

		//	2 行目 (y=509)
		AribItem p2; p2.Type = AribItemType::Position;
		p2.A = 200; p2.B = 509; p2.C = 60; p2.D = 40;
		Items.push_back(nsz); Items.push_back(p2);
		AribItem b; b.Type = AribItemType::Text; b.Text = L"下";
		b.D = 200; b.C = 240;
		Items.push_back(b);

		AribToAviUtl2Options o2 = opt;
		o2.UseBroadcastColor = false;
		std::vector<int> Drcs;
		AribCaptionLayout L;
		AribItemsToAviUtl2(Items, o2, &Drcs, &L);
		check("ruby goes to the line below it, not the one above",
			  L.Lines.size() == 2
			  && L.Lines[0].Text == L"<$字幕>上"
			  && L.Lines[1].Text == L"<$字幕></>下<!>し</>");
	}

	//	3r. **1 文字のルビを落とさない。**ルビ 20 ドットは字幅 40 の
	//	   ちょうど半分しか重ならない。「半分より大きい」にすると消える
	//	   (実測: 「然(さ)らばまた」のルビ「さ」)
	{
		std::vector<AribItem> Items;
		AribItem ssz; ssz.Type = AribItemType::Size; ssz.A = 5; ssz.B = 5;
		Items.push_back(ssz);
		AribItem rt; rt.Type = AribItemType::Text; rt.Text = L"さ";
		rt.D = 200; rt.C = 220;
		Items.push_back(rt);
		AribItem nsz; nsz.Type = AribItemType::Size; nsz.A = 10; nsz.B = 10;
		Items.push_back(nsz);
		AribItem bt; bt.Type = AribItemType::Text; bt.Text = L"然";
		bt.D = 200; bt.C = 240;
		Items.push_back(bt);
		AribItem bt2; bt2.Type = AribItemType::Text; bt2.Text = L"ら";
		bt2.D = 240; bt2.C = 280;
		Items.push_back(bt2);

		AribToAviUtl2Options o2 = opt;
		o2.UseBroadcastColor = false;
		std::vector<int> Drcs;
		check("a one-character ruby is kept",
			  AribItemsToAviUtl2(Items, o2, &Drcs)
			  == L"<$字幕></>然<!>さ</>ら");
	}

	//	3s. **MDF (CSI ... 0x20 'd') = 太字 / 斜体。**
	//	   `<@+B>` で足して `<@-B>` で外す。
	//	   **下線 (STL/SPL) と囲み (HLC) は AviUtl2 に制御文字が無い**
	//	   ので復号だけして出力には使わない
	{
		std::vector<AribItem> Items;
		AribItem b; b.Type = AribItemType::Decoration; b.A = 1;	// 太字
		AribItem t; t.Type = AribItemType::Text; t.Text = L"あ";
		AribItem n; n.Type = AribItemType::Decoration; n.A = 0;	// 標準
		AribItem t2; t2.Type = AribItemType::Text; t2.Text = L"い";
		AribItem u; u.Type = AribItemType::Decoration; u.A = 4;	// 下線
		AribItem t3; t3.Type = AribItemType::Text; t3.Text = L"う";
		Items.push_back(b); Items.push_back(t);
		Items.push_back(n); Items.push_back(t2);
		Items.push_back(u); Items.push_back(t3);

		AribToAviUtl2Options o2 = opt;
		o2.UseBroadcastColor = false;
		std::vector<int> Drcs;
		check("MDF becomes a font style, underline is dropped",
			  AribItemsToAviUtl2(Items, o2, &Drcs)
			  == L"<$字幕><@+B>あ<@-B>いう");
	}

	//	3f. **位置**。ACPS は行の下端を指すので 1 行分引いて上端にする
	{
		std::vector<AribItem> Items;
		AribItem g; g.Type = AribItemType::Geometry; g.A = 36; g.B = 540;
		AribItem p; p.Type = AribItemType::Position; p.A = 200; p.B = 509; p.C = 60;
		//	Text の C は「書き終えた後のペンの X」。送り幅 40 で 1 文字
		AribItem t; t.Type = AribItemType::Text; t.Text = L"あ"; t.C = 240;
		Items.push_back(g); Items.push_back(p); Items.push_back(t);
		std::vector<int> Drcs;
		AribCaptionLayout L;
		AribItemsToAviUtl2(Items, opt, &Drcs, &L);
		check("the caption position is reported", L.IsValid());
		check("the top is one line above the ACPS baseline",
			  L.Lines.size() == 1 && L.Lines[0].Left == 200
			  && L.Lines[0].Top == 509 + 1 - 60);
		check("the caption plane size comes from the geometry",
			  L.PlaneWidth == 960 && L.PlaneHeight == 540);
		//	**AviUtl2 のテキストは X が行の中央**なので右端が要る。
		//	実測 (X=-620 / サイズ 72 / 10 文字) で、左端ではなく
		//	中央が指定した座標に来た
		check("the line keeps its right edge for the centre",
			  L.Lines[0].Right == 200 + 40
			  && L.Lines[0].Right == 200 + 40);
	}

	//	3i. **3 行以上**。実放送に 3 行の字幕が在る (実測)。
	//	   行ごとに座標を持ち、行ごとに 1 つのオブジェクトになる
	{
		std::vector<AribItem> Items;
		AribItem g; g.Type = AribItemType::Geometry; g.A = 36; g.B = 540;
		Items.push_back(g);
		const wchar_t *Text[3] = { L"あ", L"い", L"う" };
		for (int n = 0; n < 3; n++) {
			AribItem p; p.Type = AribItemType::Position;
			p.A = 100; p.B = 389 + n * 60; p.C = 60;
			AribItem t; t.Type = AribItemType::Text;
			t.Text = Text[n]; t.C = 140;
			Items.push_back(p); Items.push_back(t);
		}
		AribToAviUtl2Options o2 = opt;
		o2.UseBroadcastColor = false;
		std::vector<int> Drcs;
		AribCaptionLayout L;
		AribItemsToAviUtl2(Items, o2, &Drcs, &L);
		check("three lines become three objects", L.Lines.size() == 3);
		bool fOk = (L.Lines.size() == 3);
		for (size_t n = 0; fOk && n < 3; n++) {
			fOk = fOk && L.Lines[n].Top == 330 + static_cast<int>(n) * 60
				  && L.Lines[n].Left == 100
				  && L.Lines[n].Text == std::wstring(L"<$字幕>") + Text[n];
		}
		check("each line keeps its own position and text", fOk);
	}

	//	3j. **同じ高さで横に飛んだら別の行にする。**
	//	   ドラマやアニメで複数の話者を同時に別の場所へ出す時に起こる
	//	   (実測: 放送 14 番組中 4 番組で発生)
	{
		std::vector<AribItem> Items;
		AribItem g; g.Type = AribItemType::Geometry; g.A = 36; g.B = 540;
		Items.push_back(g);

		//	左は x=60 に 2 文字、右は x=500 に 2 文字。高さは同じ
		const int X[2] = { 60, 500 };
		const wchar_t *T[2] = { L"あ", L"い" };
		for (int n = 0; n < 2; n++) {
			AribItem p; p.Type = AribItemType::Position;
			p.A = X[n]; p.B = 509; p.C = 60; p.D = 40;
			AribItem t; t.Type = AribItemType::Text;
			t.Text = T[n]; t.C = X[n] + 40;
			Items.push_back(p); Items.push_back(t);
		}
		AribToAviUtl2Options o2 = opt;
		o2.UseBroadcastColor = false;
		std::vector<int> Drcs;
		AribCaptionLayout L;
		AribItemsToAviUtl2(Items, o2, &Drcs, &L);
		check("a jump to the right on the same row splits the line",
			  L.Lines.size() == 2
			  && L.Lines[0].Left == 60 && L.Lines[1].Left == 500
			  && L.Lines[0].Top == L.Lines[1].Top);
	}

	//	3k. 続けて書いただけなら割らない (同じ行のまま)
	{
		std::vector<AribItem> Items;
		AribItem g; g.Type = AribItemType::Geometry; g.A = 36; g.B = 540;
		AribItem p; p.Type = AribItemType::Position;
		p.A = 60; p.B = 509; p.C = 60; p.D = 40;
		AribItem t; t.Type = AribItemType::Text; t.Text = L"あ"; t.C = 100;
		//	書き終えた所のすぐ隣に置き直す
		AribItem p2 = p; p2.A = 100;
		AribItem t2; t2.Type = AribItemType::Text; t2.Text = L"い"; t2.C = 140;
		Items.push_back(g); Items.push_back(p); Items.push_back(t);
		Items.push_back(p2); Items.push_back(t2);
		AribToAviUtl2Options o2 = opt;
		o2.UseBroadcastColor = false;
		std::vector<int> Drcs;
		AribCaptionLayout L;
		AribItemsToAviUtl2(Items, o2, &Drcs, &L);
		check("writing on beyond the pen stays one line",
			  L.Lines.size() == 1 && L.Lines[0].Text == L"<$字幕>あい");
	}

	//	3h. 行の幅は「ペンの進み」で出す。文字数 x 送り幅
	{
		//	ACPS(100,509) の後に 3 文字。送り幅は 36+4 = 40
		const BYTE d[] = {
			0x9B, 0x33, 0x36, 0x3B, 0x33, 0x36, 0x20, 0x57,
			0x9B, 0x34, 0x20, 0x58,
			0x9B, 0x31, 0x30, 0x30, 0x3B, 0x35, 0x30, 0x39, 0x20, 0x61,
			0x24, 0x22, 0x24, 0x24, 0x24, 0x26,
		};
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		std::vector<int> Drcs;
		AribCaptionLayout L;
		AribToAviUtl2Options o2 = opt;
		o2.UseBroadcastColor = false;
		AribItemsToAviUtl2(Items, o2, &Drcs, &L);
		check("the pen advance gives the line width",
			  L.Lines.size() == 1 && L.Lines[0].Left == 100
			  && L.Lines[0].Right == 100 + 3 * 40);
	}

	//	3g. ルビの位置は無視する (本文の行だけを見る)
	{
		std::vector<AribItem> Items;
		AribItem g; g.Type = AribItemType::Geometry; g.A = 36; g.B = 540;
		AribItem p; p.Type = AribItemType::Position; p.A = 200; p.B = 449; p.C = 60;
		AribItem sz; sz.Type = AribItemType::Size; sz.A = 5; sz.B = 5;
		AribItem r; r.Type = AribItemType::Text; r.Text = L"る";
		AribItem p2; p2.Type = AribItemType::Position; p2.A = 80; p2.B = 509; p2.C = 60;
		AribItem n; n.Type = AribItemType::Size; n.A = 10; n.B = 10;
		AribItem t; t.Type = AribItemType::Text; t.Text = L"あ";
		Items.push_back(g); Items.push_back(p); Items.push_back(sz); Items.push_back(r);
		Items.push_back(p2); Items.push_back(n); Items.push_back(t);
		std::vector<int> Drcs;
		AribCaptionLayout L;
		AribItemsToAviUtl2(Items, opt, &Drcs, &L);
		check("a ruby line does not move the caption position",
			  L.Lines.size() == 1 && L.Lines[0].Left == 80
			  && L.Lines[0].Top == 509 + 1 - 60);
	}

	//	3d. **位置が 1 行下がったら改行**。放送は改行を送って来ない
	{
		std::vector<AribItem> Items;
		AribItem p; p.Type = AribItemType::Position; p.A = 80; p.B = 509; p.C = 60;
		AribItem t; t.Type = AribItemType::Text; t.Text = L"あ";
		AribItem p2 = p; p2.B = 569;
		AribItem t2 = t; t2.Text = L"い";
		Items.push_back(p); Items.push_back(t);
		Items.push_back(p2); Items.push_back(t2);
		AribToAviUtl2Options o2 = opt;
		o2.UseBroadcastColor = false;
		std::vector<int> Drcs;
		AribCaptionLayout L;
		AribItemsToAviUtl2(Items, o2, &Drcs, &L);
		//	**行ごとに別のオブジェクトになる。**放送は行ごとに座標を
		//	持っているので、まとめると位置も背景の箱も合わなくなる
		check("a line below becomes a separate line",
			  L.Lines.size() == 2
			  && L.Lines[0].Text == L"<$字幕>あ" && L.Lines[0].Top == 450
			  && L.Lines[1].Text == L"<$字幕>い" && L.Lines[1].Top == 510);
	}

	//	3e. **ルビは行として数えない**。ルビは本文の 1 行上に先に書かれる
	{
		std::vector<AribItem> Items;
		AribItem p; p.Type = AribItemType::Position; p.A = 200; p.B = 449; p.C = 60;
		AribItem sz; sz.Type = AribItemType::Size; sz.A = 5; sz.B = 5;	// 小型 = ルビ
		AribItem r; r.Type = AribItemType::Text; r.Text = L"る";
		r.D = 200; r.C = 220;			// 小型は送り 20
		AribItem p2 = p; p2.B = 509;
		AribItem n; n.Type = AribItemType::Size; n.A = 10; n.B = 10;
		AribItem t; t.Type = AribItemType::Text; t.Text = L"あ";
		t.D = 200; t.C = 240;
		Items.push_back(p); Items.push_back(sz); Items.push_back(r);
		Items.push_back(p2); Items.push_back(n); Items.push_back(t);
		AribToAviUtl2Options o2 = opt;
		o2.UseBroadcastColor = false;
		std::vector<int> Drcs;
		//	**1 行に収まる事。**ルビで行が割れると
		//	「ルビ / 本文」が毎回別のオブジェクトになってしまう
		AribCaptionLayout L;
		const std::wstring s = AribItemsToAviUtl2(Items, o2, &Drcs, &L);
		check("a ruby line does not become a line break",
			  L.Lines.size() == 1 && s == L"<$字幕></>あ<!>る</>");
	}

	//	4. 外字。1 文字だけフォントを切り替え、本文の書体は保つ
	{
		std::vector<AribItem> Items;
		AribItem d; d.Type = AribItemType::Drcs; d.A = 0x4121;
		AribItem t; t.Type = AribItemType::Text; t.Text = L"あ";
		Items.push_back(d); Items.push_back(t); Items.push_back(d);
		std::vector<int> Drcs;
		const std::wstring s = AribItemsToAviUtl2(Items, opt, &Drcs);
		check("a DRCS switches font for one character only",
			  s == L"<$字幕><@TSMemoryDRCS><@>あ<@TSMemoryDRCS><@>");
		check("the same glyph gets the same code", Drcs.size() == 1);
	}

	//	4b. **既定のフォント名 (空白入り) でも同じ形になる事。**
	//	   `[Caption] DrcsFont` の既定は "TSMemory DRCS"。
	//	   `<@名前,装飾>` の区切りは ',' なので空白は名前の一部になる
	{
		std::vector<AribItem> Items;
		AribItem d; d.Type = AribItemType::Drcs; d.A = 0x4121;
		Items.push_back(d);
		AribToAviUtl2Options o2 = opt;
		o2.DrcsFont = L"TSMemory DRCS";
		std::vector<int> Drcs;
		const std::wstring s = AribItemsToAviUtl2(Items, o2, &Drcs);

		//	<@TSMemory DRCS> + U+E000 + <@>
		std::wstring Want = L"<$字幕><@TSMemory DRCS>";
		Want += static_cast<wchar_t>(0xE000);
		Want += L"<@>";
		check("the default font name with a space is emitted as is", s == Want);

		//	**私用領域の 1 文字が挟まっている事。**
		//	ここが抜けると外字の位置に何も出ない
		check("a private-use character sits between the font switches",
			  s.find(static_cast<wchar_t>(0xE000)) != std::wstring::npos);
	}

	//	5. 外字用フォントが無ければ代替文字
	{
		std::vector<AribItem> Items;
		AribItem d; d.Type = AribItemType::Drcs; d.A = 0x4121;
		Items.push_back(d);
		AribToAviUtl2Options o2 = opt;
		o2.DrcsFont.clear();
		std::vector<int> Drcs;
		check("without a DRCS font a placeholder is used",
			  AribItemsToAviUtl2(Items, o2, &Drcs) == L"<$字幕>〓");
	}

	//	6. **本文の '<' は全角に置き換える。**
	//	   AviUtl2 の仕様に打ち消し方が書かれていないので、
	//	   `<<` のような当て推量は使わない。字幕の本文はもともと全角で、
	//	   英数集合も全角で出しているので ASCII の '<' はまず来ない
	{
		std::vector<AribItem> Items;
		AribItem t; t.Type = AribItemType::Text; t.Text = L"<笑い>";
		Items.push_back(t);
		AribToAviUtl2Options o2 = opt;
		o2.Preset.clear();
		std::vector<int> Drcs;
		check("'<' in the text is replaced with the fullwidth one",
			  AribItemsToAviUtl2(Items, o2, &Drcs) == L"＜笑い>");
	}

	//	7. 色の対応
	{
		check("colour 7 is white", AribColorToRgb(7) == 0xFFFFFF);
		check("colour 0 is black", AribColorToRgb(0) == 0x000000);
		check("colour 3 is yellow", AribColorToRgb(3) == 0xFFFF00);
	}

	std::printf("\n");
}

}	// namespace


int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	RunUnitTests();
	RunConvertTests();

	const char *pszPath = argc > 1 ? argv[1] : nullptr;
	const bool fDump = argc > 2 && std::strcmp(argv[2], "--dump") == 0;
	std::wstring Find;
	if (argc > 3) {
		//	**argv は ANSI (日本語環境なら CP932)。**
		//	UTF-8 で変換すると日本語が化けて一致しなくなる
		const int n = ::MultiByteToWideChar(CP_ACP, 0, argv[3], -1, nullptr, 0);
		if (n > 1) {
			Find.resize(n - 1);
			::MultiByteToWideChar(CP_ACP, 0, argv[3], -1, &Find[0], n);
			g_pszFind = Find.c_str();
		}
	}
	if (pszPath == nullptr) {
		std::printf("%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
					g_failures, g_failures == 1 ? "" : "s");
		return g_failures == 0 ? 0 : 1;
	}

	std::vector<BYTE> Ts;
	{
		FILE *fp = std::fopen(pszPath, "rb");
		if (fp == nullptr) {
			std::printf("cannot open %s\n", pszPath);
			return 1;
		}
		//	**ftell は long (32bit) なので 2GB を超える TS で溢れる。**
		//	放送の録画は普通に数 GB あるので 64bit で測る
		_fseeki64(fp, 0, SEEK_END);
		const long long Size = _ftelli64(fp);
		_fseeki64(fp, 0, SEEK_SET);

		//	全部読む必要は無い。字幕は全編に散っているので先頭の一部で足りる。
		//	**見たい字幕が先頭に無い事もある**ので、環境変数で
		//	読む範囲を動かせるようにしてある (実データの不具合を追う時用)
		//	  TSMEMORY_TS_OFFSET_MB … 何 MB 目から読むか
		//	  TSMEMORY_TS_LIMIT_MB  … 何 MB 読むか
		long long Offset = 0;
		long long Limit = 512LL * 1024 * 1024;
		if (const char *p = std::getenv("TSMEMORY_TS_OFFSET_MB"))
			Offset = ::atoll(p) * 1024 * 1024 / 188 * 188;
		if (const char *p = std::getenv("TSMEMORY_TS_LIMIT_MB"))
			Limit = ::atoll(p) * 1024 * 1024;
		if (Offset > Size)
			Offset = 0;
		_fseeki64(fp, Offset, SEEK_SET);

		const long long Rest = Size - Offset;
		const size_t Want = static_cast<size_t>(Rest < Limit ? Rest : Limit);
		Ts.resize(Want);
		Ts.resize(std::fread(Ts.data(), 1, Ts.size(), fp));
		std::fclose(fp);
		std::printf("read %.0f MB of %.0f MB (offset %.0f MB)\n",
					Ts.size() / 1048576.0, Size / 1048576.0,
					Offset / 1048576.0);
	}
	std::printf("TS file : %s (%zu packets)\n", pszPath, Ts.size() / TS_PACKET_SIZE);

	const std::vector<CaptionPid> Pids = FindCaptionPids(Ts);
	std::printf("caption PIDs :");
	for (const CaptionPid &c : Pids)
		std::printf(" 0x%04X%s", c.Pid, c.fSuperimpose ? "(超)" : "(字幕)");
	std::printf("\n\n");
	//	字幕を持たない TS (合成した物や CM だけの切り出し) は検査対象外。
	//	**「字幕が無い」と「復号に失敗した」を混同しない**為、
	//	ここで skip して 0 を返す
	if (Pids.empty()) {
		std::printf("this TS has no caption stream - skipped\n");
		return g_failures == 0 ? 0 : 1;
	}

	//	--- 字幕の PES を組み立てて復号する ---------------------------------
	std::map<WORD, std::vector<BYTE>> Bufs;
	std::vector<CaptionUnit> Units;

	auto Flush = [&](WORD Pid) {
		auto it = Bufs.find(Pid);
		if (it != Bufs.end() && !it->second.empty())
			ParseCaptionPes(it->second, &Units);
	};

	for (size_t i = 0; i + TS_PACKET_SIZE <= Ts.size(); i += TS_PACKET_SIZE) {
		const BYTE *p = &Ts[i];
		if (p[0] != 0x47)
			continue;
		const WORD Pid = static_cast<WORD>(((p[1] & 0x1F) << 8) | p[2]);
		bool fTarget = false;
		for (const CaptionPid &c : Pids)
			fTarget = fTarget || (c.Pid == Pid && !c.fSuperimpose);
		if (!fTarget || (p[3] & 0x10) == 0)
			continue;

		size_t o = 4;
		if (p[3] & 0x20)
			o += 1 + p[4];
		if (o >= TS_PACKET_SIZE)
			continue;

		if (p[1] & 0x40) {
			Flush(Pid);
			Bufs[Pid].assign(p + o, p + TS_PACKET_SIZE);
		} else if (Bufs.count(Pid)) {
			Bufs[Pid].insert(Bufs[Pid].end(), p + o, p + TS_PACKET_SIZE);
		}
	}
	for (const auto &e : Bufs)
		Flush(e.first);

	int Body = 0, Drcs = 0;
	//	定義された外字の符号 (**本文と同じ形に直した値**)
	std::map<int, int> DrcsDefined;
	for (const CaptionUnit &u : Units) {
		if (u.Parameter == 0x20) Body++;
		if (u.Parameter == 0x30 || u.Parameter == 0x31) {
			Drcs++;
			//	**定義側の符号を本文側と同じ形に直して数える。**
			//	1 バイト外字 (0x30) の character_code は
			//	「上位 = 集合 / 下位 = 集合の中の符号」なので、
			//	そのまま 2 バイトで持つと本文の 0x21 と結び付かない。
			//	ここが合っているかを実データで見る
			if (u.Body.size() >= 4) {
				const int Raw = (u.Body[1] << 8) | u.Body[2];
				const int Code = (u.Parameter == 0x30)
								 ? (Raw & 0x7F)
								 : ((Raw >= 0xEC00 && Raw <= 0xF8FF)
									? Raw : (Raw & 0x7F7F));
				DrcsDefined[Code]++;
			}
		}
	}
	std::printf("data units : 本文 %d / DRCS %d (合計 %zu)\n", Body, Drcs, Units.size());
	if (!DrcsDefined.empty()) {
		std::printf("  定義された外字の符号 :");
		for (const auto &e : DrcsDefined)
			std::printf(" 0x%04X(%d件)", e.first, e.second);
		std::printf("\n");
	}
	std::printf("\n");

	//	**本文が無い時こそ生バイトが要る。**復号に失敗しているのか
	//	本当に画面消去だけなのかは、これを見ないと切り分けられない
	if (fDump) {
		std::printf("--- データユニットの生バイト (先頭 6 件) ---\n");
		int n = 0;
		for (const CaptionUnit &u : Units) {
			//	画面消去だけの 1 バイトの物は見ても仕方がない
			if (u.Body.size() <= 4 && Units.size() > 6)
				continue;
			//	**検索文字列があれば、それを含む物だけ出す。**
			//	先頭 6 件しか出さないので、追いたい字幕に届かない
			if (g_pszFind != nullptr) {
				std::vector<AribItem> It;
				AribDecodeText(u.Body.data(), u.Body.size(), &It);
				if (AribItemsToPlainText(It).find(g_pszFind) == std::wstring::npos)
					continue;
			}
			if (++n > 6)
				break;
			std::printf("  [param %02X / %zu bytes]", u.Parameter, u.Body.size());
			for (size_t i = 0; i < u.Body.size() && i < 400; i++)
				std::printf(" %02X", u.Body[i]);
			std::printf("\n");
		}
		std::printf("\n");
	}

	//	字幕 PID はあるが本文が無い切り出し (画面消去だけ等) も検査対象外
	if (Body == 0) {
		std::printf("no caption text data unit in this range - skipped\n");
		return g_failures == 0 ? 0 : 1;
	}

	//	--- 復号 -------------------------------------------------------------
	int Decoded = 0, WithText = 0, DrcsRefs = 0, Positions = 0, Colors = 0;
	//	**縁取り (ORN) は放送が送って来るのか。**
	//	TVCaptionMod2 は既定 (StrokeWidth=-2) で全ての字幕に縁を付け、
	//	ORN が来た時だけ太さを OrnStrokeWidth に替えている。
	//	どちらが効いているのかを実測で切り分ける為に数える
	int Ornaments = 0;
	std::map<int, int> OrnColors;
	//	**放送は改行符号 (APR 0x0D / APD 0x0A) を送って来るのか。**
	//	送って来ないなら、行の区切りは ACPS の座標からしか起こせない。
	//	「2 行の字幕は TS の上でも 2 行なのか」を切り分ける為に数える
	int LineBreaks = 0;
	int Rubies = 0;
	//	**外字の符号。**二重かっこ等が化ける件の切り分け用。
	//	字形が届かないと代替文字になるので、どの符号が来ているかを見る
	std::map<int, int> DrcsCodes;
	//	**同じ高さで横に離れた位置に書き直した回数。**
	//	複数の話者を同時に別の場所へ出す字幕で起こる。
	//	行の区切りを Y だけで見ていると 1 行に繋がってしまう
	int SideBySide = 0;
	//	変換の結果、同じ高さで 2 つ以上の行に割れた回数
	int SplitSameRow = 0;
	int TotalLines = 0;
	size_t TotalChars = 0;
	std::vector<std::wstring> Samples, Converted;
	std::vector<AribCaptionLayout> Layouts;

	for (const CaptionUnit &u : Units) {
		if (u.Parameter != 0x20)
			continue;
		std::vector<AribItem> Items;
		AribDecodeText(u.Body.data(), u.Body.size(), &Items);
		Decoded++;

		int PrevY = -1, Pen = -1;
		int ScaleH = 10, ScaleV = 10;
		for (const AribItem &it : Items) {
			if (it.Type == AribItemType::Drcs) {
				DrcsRefs++;
				DrcsCodes[it.A]++;
			}
			if (it.Type == AribItemType::Color) Colors++;
			if (it.Type == AribItemType::LineBreak) LineBreaks++;
			//	**小型 (SSZ) で書かれた文字 = ルビ。**
			//	大きさの指定そのものを数えてはいけない。位置指定の前に
			//	SSZ を置いてすぐ MSZ に戻す放送があり (実測)、
			//	それを数えると**文字が 1 つも無いのにルビ有りになる**
			if (it.Type == AribItemType::Size) {
				ScaleH = it.A;
				ScaleV = it.B;
			}
			if ((it.Type == AribItemType::Text || it.Type == AribItemType::Drcs)
					&& ScaleH == 5 && ScaleV == 5)
				Rubies++;
			if (it.Type == AribItemType::Ornament && it.A == 1) {
				Ornaments++;
				OrnColors[it.B]++;
			}
			if (it.Type == AribItemType::Text || it.Type == AribItemType::Drcs) {
				if (it.C > 0)
					Pen = it.C;
			}
			if (it.Type != AribItemType::Position)
				continue;
			Positions++;
			//	**同じ高さで、書き終えた所より右へ飛んだ。**
			//	複数の話者を同時に別の場所へ出す字幕で起こる
			if (PrevY >= 0 && it.B == PrevY && Pen > 0 && it.A > Pen + 40)
				SideBySide++;
			PrevY = it.B;
			Pen = it.A;
		}

		//	**全ての字幕文について**行の割れ方を数える (表示は先頭 12 件だけ)
		{
			AribToAviUtl2Options o;
			std::vector<int> Codes;
			AribCaptionLayout L;
			AribItemsToAviUtl2(Items, o, &Codes, &L);
			TotalLines += static_cast<int>(L.Lines.size());
			for (size_t n = 1; n < L.Lines.size(); n++) {
				if (L.Lines[n].Top == L.Lines[n - 1].Top)
					SplitSameRow++;
			}
		}

		const std::wstring s = AribItemsToPlainText(Items);
		//	改行と空白だけの物は数えない
		bool fHasChar = false;
		for (wchar_t c : s)
			fHasChar = fHasChar || (c != L'\n' && c != L' ');
		if (fHasChar) {
			WithText++;
			TotalChars += s.size();
			//	**探したい字幕を第 3 引数で絞れる。**
			//	先頭 12 件しか出さないので、実データの不具合を
			//	追う時に見たい字幕が出て来ない
			const bool fMatch = (g_pszFind == nullptr)
								|| (s.find(g_pszFind) != std::wstring::npos);
			if (fMatch && Samples.size() < 12) {
				Samples.push_back(s);
				//	AviUtl2 のテキストに直した形も見る
				AribToAviUtl2Options opt;
				opt.Preset = L"字幕";
				opt.DrcsFont = L"TSMemoryDRCS";
				std::vector<int> Codes;
				AribCaptionLayout L;
				Converted.push_back(AribItemsToAviUtl2(Items, opt, &Codes, &L));
				Layouts.push_back(L);
			}
		}
	}

	std::printf("decoded %d units : 本文あり %d / 文字数 %zu\n", Decoded, WithText, TotalChars);
	std::printf("  制御 : 位置 %d / 改行 %d / 色 %d / 外字 %d / ルビ(小型) %d"
				" / 同じ高さで横に飛んだ %d\n",
				Positions, LineBreaks, Colors, DrcsRefs, Rubies, SideBySide);
	std::printf("  行 : 合計 %d / うち同じ高さで割れた %d\n",
				TotalLines, SplitSameRow);
	if (!DrcsCodes.empty()) {
		std::printf("  外字の符号 :");
		int n = 0;
		for (const auto &e : DrcsCodes) {
			if (++n > 8) { std::printf(" ..."); break; }
			std::printf(" 0x%04X(%d件)", e.first, e.second);
		}
		std::printf("\n");
	}
	std::printf("  縁取り (ORN) : %d 件", Ornaments);
	for (const auto &e : OrnColors)
		std::printf(" / 色 %d が %d 件", e.first, e.second);
	std::printf("\n");

	//	**表示の長さは「消す」ユニットから決まる。**
	//	`CTSCaptionSource` と同じ規則をここでもなぞって、
	//	消える時刻をいくつ拾えるか / 「次の字幕まで」で代用した場合と
	//	どれだけ違うかを出す。
	//
	//	規則 : 画面消去 (CS) を含むか本文のあるデータユニットが来たら、
	//	       それまでに出ていた字幕はそこで消える
	{
		struct Shown { double Start; double End; double Next; };
		std::vector<Shown> Live;
		double SumShown = 0.0, SumNext = 0.0;
		int Closed = 0, Open = 0, Late = 0;
		double Worst = 0.0;

		for (size_t i = 0; i < Units.size(); i++) {
			if (Units[i].Parameter != 0x20 || Units[i].Pts < 0)
				continue;
			std::vector<AribItem> Items;
			AribDecodeText(Units[i].Body.data(), Units[i].Body.size(), &Items);
			AribToAviUtl2Options o;
			std::vector<int> Codes;
			AribCaptionLayout L;
			AribItemsToAviUtl2(Items, o, &Codes, &L);

			bool fClear = false;
			for (const AribItem &it : Items)
				fClear = fClear || (it.Type == AribItemType::ClearScreen);

			const double t = Units[i].Pts / 90000.0;

			//	**ここで前の字幕が消える。**
			//	本文が空でも CS が無ければ消さない (属性だけの指定)
			if (fClear || !L.Lines.empty()) {
				for (Shown &s : Live) {
					if (s.End < 0.0 && s.Start < t)
						s.End = t;
				}
			}
			//	「次の字幕まで」で代用した場合の終わり
			if (!L.Lines.empty()) {
				for (Shown &s : Live) {
					if (s.Next < 0.0 && s.Start < t)
						s.Next = t;
				}
				Shown s = { t, -1.0, -1.0 };
				Live.push_back(s);
			}
		}

		for (const Shown &s : Live) {
			if (s.End < 0.0) { Open++; continue; }
			Closed++;
			const double Shownf = s.End - s.Start;
			const double Nextf = (s.Next > s.Start) ? s.Next - s.Start : Shownf;
			SumShown += Shownf;
			SumNext += Nextf;
			if (Nextf - Shownf > Worst)
				Worst = Nextf - Shownf;
			if (Nextf - Shownf > 0.5)
				Late++;
		}
		//	**大半の字幕で消える時刻が拾えている事。**
		//	CS の判定が壊れると Closed が 0 に落ちて、
		//	字幕が次の字幕まで出しっぱなしに戻る
		if (!Live.empty())
			check("most captions have an explicit erase time", Closed >= Open);

		if (Closed > 0) {
			std::printf("  表示の長さ : %d 件で消える時刻が判った "
						"(判らず次の字幕まで %d 件)\n"
						"    放送 平均 %.1f 秒 / 次の字幕まで 平均 %.1f 秒 "
						"(%d 件中 %d 件が 0.5 秒以上長い、最大 %.1f 秒)\n",
						Closed, Open, SumShown / Closed, SumNext / Closed,
						Closed, Late, Worst);
		}
	}
	std::printf("\n");

	//	本文はあるが画面消去だけ、という区間もある (短い切り出しで起こる)
	if (WithText == 0) {
		std::printf("the caption units carry no text (clear only) - skipped\n");
		return g_failures == 0 ? 0 : 1;
	}

	check("some units decoded into text", WithText > 0);
	check("the decoded text is not trivially short", TotalChars > static_cast<size_t>(WithText));

	//	日本語が出ているか (ひらがな・カタカナ・漢字のいずれか)
	bool fJapanese = false;
	for (const std::wstring &s : Samples) {
		for (wchar_t c : s) {
			if ((c >= 0x3040 && c <= 0x30FF) || (c >= 0x4E00 && c <= 0x9FFF))
				fJapanese = true;
		}
	}
	check("the text contains Japanese characters", fJapanese);

	std::printf("--- 復号した字幕 (先頭 %zu 件) ---\n", Samples.size());
	for (size_t k = 0; k < Samples.size(); k++) {
		const std::wstring &s = k < Converted.size() ? Converted[k] : Samples[k];
		//	端末に出す為 UTF-8 に直す
		const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
		std::vector<char> u8(n > 0 ? n : 1);
		::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, u8.data(), n, nullptr, nullptr);
		if (k < Layouts.size()) {
			//	1920x1080 に置いた時の左上 (画面中央が原点)
			for (const AribCaptionLine &Line : Layouts[k].Lines) {
				//	**基準点は行の左上。**テキストオブジェクトの
				//	文字揃え「左寄せ[上]」がそのままの意味だった
				//	(実機で TVCaptionMod2 と並べて確認済み)
				const int X = Line.Left * 1920 / Layouts[k].PlaneWidth - 960;
				const int Y = Line.Top * 1080 / Layouts[k].PlaneHeight - 540;
				std::printf("  (左%3d 右%3d 上%3d dots -> 1080p X=%d Y=%d)\n",
							Line.Left, Line.Right, Line.Top, X, Y);
			}
		}
		std::printf("  [%s]\n", u8.data());
	}

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
				g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
