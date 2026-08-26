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
};

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

	//	7. 英数 (GL を G1 に切り替える LS1)
	{
		const BYTE d[] = { 0x0E, 0x41, 0x42, 0x43 };
		std::vector<AribItem> Items;
		AribDecodeText(d, sizeof(d), &Items);
		check("LS1 switches to the alphanumeric set",
			  AribItemsToPlainText(Items) == L"ABC");
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

}	// namespace


int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	RunUnitTests();

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

	//	字幕 PID はあるが本文が無い切り出し (画面消去だけ等) も検査対象外
	if (Body == 0) {
		std::printf("no caption text data unit in this range - skipped\n");
		return g_failures == 0 ? 0 : 1;
	}

	//	--- 復号 -------------------------------------------------------------
	int Decoded = 0, WithText = 0, DrcsRefs = 0, Positions = 0, Colors = 0;
	size_t TotalChars = 0;
	std::vector<std::wstring> Samples;

	for (const CaptionUnit &u : Units) {
		if (u.Parameter != 0x20)
			continue;
		std::vector<AribItem> Items;
		AribDecodeText(u.Body.data(), u.Body.size(), &Items);
		Decoded++;

		for (const AribItem &it : Items) {
			if (it.Type == AribItemType::Drcs) DrcsRefs++;
			if (it.Type == AribItemType::Position) Positions++;
			if (it.Type == AribItemType::Color) Colors++;
		}

		const std::wstring s = AribItemsToPlainText(Items);
		//	改行と空白だけの物は数えない
		bool fHasChar = false;
		for (wchar_t c : s)
			fHasChar = fHasChar || (c != L'\n' && c != L' ');
		if (fHasChar) {
			WithText++;
			TotalChars += s.size();
			if (Samples.size() < 12)
				Samples.push_back(s);
		}
	}

	std::printf("decoded %d units : 本文あり %d / 文字数 %zu\n", Decoded, WithText, TotalChars);
	std::printf("  制御 : 位置 %d / 色 %d / 外字 %d\n\n", Positions, Colors, DrcsRefs);

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
	for (const std::wstring &s : Samples) {
		//	端末に出す為 UTF-8 に直す
		const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
		std::vector<char> u8(n > 0 ? n : 1);
		::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, u8.data(), n, nullptr, nullptr);
		std::printf("  [%s]\n", u8.data());
	}

	if (fDump) {
		std::printf("\n--- 本文の生バイト (先頭 6 件) ---\n");
		int n = 0;
		for (const CaptionUnit &u : Units) {
			if (u.Parameter != 0x20 || u.Body.size() < 8)
				continue;
			if (++n > 2)
				break;
			std::printf("  [%zu bytes]", u.Body.size());
			for (size_t i = 0; i < u.Body.size() && i < 120; i++)
				std::printf(" %02X", u.Body[i]);
			std::printf("\n");
		}
	}
	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
				g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
