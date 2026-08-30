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
			  && L.Lines[0].CenterX() == 200 + 20);
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
		AribItem p2 = p; p2.B = 509;
		AribItem n; n.Type = AribItemType::Size; n.A = 10; n.B = 10;
		AribItem t; t.Type = AribItemType::Text; t.Text = L"あ";
		Items.push_back(p); Items.push_back(sz); Items.push_back(r);
		Items.push_back(p2); Items.push_back(n); Items.push_back(t);
		AribToAviUtl2Options o2 = opt;
		o2.UseBroadcastColor = false;
		std::vector<int> Drcs;
		//	小型は縦横とも半分。相対指定の基準が仕様に無いので
		//	一度 <s> で戻してから掛ける
		check("a ruby line does not become a line break",
			  AribItemsToAviUtl2(Items, o2, &Drcs) == L"<$字幕><s><s*0.5>る<s>あ");
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

		//	全部読む必要は無い。字幕は全編に散っているので先頭の一部で足りる
		const long long Limit = 512LL * 1024 * 1024;
		const size_t Want = static_cast<size_t>(Size < Limit ? Size : Limit);
		Ts.resize(Want);
		Ts.resize(std::fread(Ts.data(), 1, Ts.size(), fp));
		std::fclose(fp);
		std::printf("read %.0f MB of %.0f MB\n",
					Ts.size() / 1048576.0, Size / 1048576.0);
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
	for (const CaptionUnit &u : Units) {
		if (u.Parameter == 0x20) Body++;
		if (u.Parameter == 0x30 || u.Parameter == 0x31) Drcs++;
	}
	std::printf("data units : 本文 %d / DRCS %d (合計 %zu)\n\n", Body, Drcs, Units.size());

	//	**本文が無い時こそ生バイトが要る。**復号に失敗しているのか
	//	本当に画面消去だけなのかは、これを見ないと切り分けられない
	if (fDump) {
		std::printf("--- データユニットの生バイト (先頭 6 件) ---\n");
		int n = 0;
		for (const CaptionUnit &u : Units) {
			//	画面消去だけの 1 バイトの物は見ても仕方がない
			if (u.Body.size() <= 4 && Units.size() > 6)
				continue;
			if (++n > 6)
				break;
			std::printf("  [param %02X / %zu bytes]", u.Parameter, u.Body.size());
			for (size_t i = 0; i < u.Body.size() && i < 160; i++)
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
		for (const AribItem &it : Items) {
			if (it.Type == AribItemType::Drcs) DrcsRefs++;
			if (it.Type == AribItemType::Color) Colors++;
			if (it.Type == AribItemType::LineBreak) LineBreaks++;
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
			if (Samples.size() < 12) {
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
	std::printf("  制御 : 位置 %d / 改行 %d / 色 %d / 外字 %d"
				" / 同じ高さで横に飛んだ %d\n",
				Positions, LineBreaks, Colors, DrcsRefs, SideBySide);
	std::printf("  行 : 合計 %d / うち同じ高さで割れた %d\n",
				TotalLines, SplitSameRow);
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
				//	**X には行の左端を入れる。**
				//	AviUtl2 のテキストは X が行の中央を指すが、
				//	描画後の幅ぶんはスクリプト側でずらして左揃えにする
				const int X = Line.Left * 1920 / Layouts[k].PlaneWidth - 960;
				const int Y = Line.Top * 1080 / Layouts[k].PlaneHeight - 540;
				std::printf("  (左%3d 右%3d 中央%3d 上%3d dots -> 1080p X=%d Y=%d)\n",
							Line.Left, Line.Right, Line.CenterX(), Line.Top, X, Y);
			}
		}
		std::printf("  [%s]\n", u8.data());
	}

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
				g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
