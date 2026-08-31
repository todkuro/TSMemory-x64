//----------------------------------------------------------------------------
//	*.tvtv の字幕 (ts_caption.h を参照)
//----------------------------------------------------------------------------
#include <windows.h>

#define STRSAFE_NO_DEPRECATE
#include <strsafe.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "ts_caption.h"
#include "arib_text.h"
#include "drcs_store.h"
#include "drcs_replace.h"

namespace {

const int TS_PACKET_SIZE = 188;

//	共有メモリのリングバッファを線形に読む (音声側と同じ規約)
bool ReadSharedMemory(const char *pszName, std::vector<BYTE> *pOut)
{
	char szMutexName[MAX_PATH];
	std::snprintf(szMutexName, sizeof(szMutexName), "%s.mutex", pszName);

	HANDLE hMutex = ::OpenMutexA(SYNCHRONIZE, FALSE, szMutexName);
	if (hMutex == nullptr)
		return false;

	if (::WaitForSingleObject(hMutex, 3000) != WAIT_OBJECT_0) {
		::CloseHandle(hMutex);
		return false;
	}

	bool fOK = false;
	HANDLE hMap = ::OpenFileMappingA(FILE_MAP_READ, FALSE, pszName);
	if (hMap != nullptr) {
		const BYTE *pView = static_cast<const BYTE *>(
			::MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0));
		if (pView != nullptr) {
			const DWORD *pInfo = reinterpret_cast<const DWORD *>(pView);
			const DWORD Size = pInfo[0];
			const DWORD Used = pInfo[1];
			const DWORD Pos = pInfo[2];
			const BYTE *pData = pView + sizeof(DWORD) * 4;

			if (Used > 0 && Size > 0 && Pos < Size && Used <= Size) {
				pOut->resize(Used);
				const DWORD First = Used < (Size - Pos) ? Used : (Size - Pos);
				::CopyMemory(pOut->data(), pData + Pos, First);
				if (First < Used)
					::CopyMemory(pOut->data() + First, pData, Used - First);
				fOK = true;
			}
			::UnmapViewOfFile(pView);
		}
		::CloseHandle(hMap);
	}

	::ReleaseMutex(hMutex);
	::CloseHandle(hMutex);
	return fOK;
}

//	PES ヘッダから PTS を取り出す (無ければ -1)
int64_t PtsOf(const BYTE *p, size_t Size)
{
	if (Size < 14 || (p[7] & 0x80) == 0)
		return -1;
	const BYTE *q = p + 9;
	return (static_cast<int64_t>((q[0] >> 1) & 0x07) << 30)
		 | (static_cast<int64_t>(q[1]) << 22)
		 | (static_cast<int64_t>(q[2] >> 1) << 15)
		 | (static_cast<int64_t>(q[3]) << 7)
		 | (static_cast<int64_t>(q[4] >> 1));
}

//	33bit で折り返す PTS の差を秒で返す
double PtsDiffSeconds(int64_t From, int64_t To)
{
	int64_t d = To - From;
	if (d < -(1LL << 32)) d += (1LL << 33);
	if (d > (1LL << 32))  d -= (1LL << 33);
	return static_cast<double>(d) / 90000.0;
}

struct Pids {
	std::vector<WORD> Caption;		// component_tag 0x30-0x37
	WORD Video = 0;
	//	PMT の program_number = サービス ID。
	//	**字幕 PID だけでは足りない。**別のチャンネルでも同じ番号
	//	(0x0110 等) を使っている事が多く、外字の字形を取り違える
	WORD Service = 0;
};

//---------------------------------------------------------------------------
//	外字の字形を取り込みをまたいで覚えておく
//
//	**字形は数十秒おきにしか流れて来ない** (実測の中央値 14.2 秒、
//	最大 210 秒)。既定の MemorySize は 8〜14 秒相当なので、
//	半々くらいで窓に入らず `《` `》` 等が代替文字になる。
//	同じチャンネルで 2 回目以降の取り込みなら前の物を使い回せる。
//
//	**符号 (0x21 から順) の意味は番組ごとに変わる。**
//	番組が変われば `0x21` が別の字形に割り当て直されるので、
//	  ・**サービス ID と字幕 PID**ごとに分ける (チャンネルが変われば別)
//	  ・古い物は使わない (下記の時間で切る)
//	の 2 つで古い字形を掴まないようにしている。
//	それでも同じチャンネルで番組をまたぐと取り違え得るので、
//	使った時はログに出す。
const DWORD GLYPH_CACHE_LIFE_MS = 30 * 60 * 1000;	// 30 分
const size_t GLYPH_CACHE_MAX = 1024;

struct CachedGlyph {
	TSMemoryDrcsGlyph Glyph;
	DWORD Tick;
};

//	キーは (サービス ID << 32) | (字幕 PID << 16) | 符号。
//
//	**サービス ID まで入れる事。**字幕 PID は別のチャンネルでも
//	同じ番号 (0x0110 等) を使っている事が多く、PID だけを鍵にすると
//	チャンネルを変えた後に前のチャンネルの字形を引いてしまう
std::map<UINT64, CachedGlyph> g_GlyphCache;

UINT64 GlyphKey(WORD Service, WORD Pid, int Code)
{
	return (static_cast<UINT64>(Service) << 32)
		 | (static_cast<UINT64>(Pid) << 16)
		 | (static_cast<UINT64>(Code) & 0xFFFF);
}

//	PAT/PMT から字幕と映像の PID を拾う
Pids FindPids(const std::vector<BYTE> &Ts)
{
	std::vector<WORD> PmtPids;
	Pids Out;

	for (size_t i = 0; i + TS_PACKET_SIZE <= Ts.size(); i += TS_PACKET_SIZE) {
		const BYTE *p = &Ts[i];
		if (p[0] != 0x47 || (p[1] & 0x40) == 0 || (p[3] & 0x10) == 0)
			continue;
		const WORD Pid = static_cast<WORD>(((p[1] & 0x1F) << 8) | p[2]);

		size_t o = 4;
		if (p[3] & 0x20)
			o += 1 + p[4];
		if (o >= TS_PACKET_SIZE)
			continue;
		o += 1 + p[o];
		if (o + 12 > TS_PACKET_SIZE)
			continue;

		//	セクションが 1 パケットに収まらない事がある。
		//	丸ごと捨てず、収まっている範囲だけ読む
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
			End = TS_PACKET_SIZE;
		if (Out.Service == 0)
			Out.Service = static_cast<WORD>((p[o + 3] << 8) | p[o + 4]);
		const size_t InfoLen = ((p[o + 10] & 0x0F) << 8) | p[o + 11];
		size_t q = o + 12 + InfoLen;
		while (q + 5 <= End) {
			const BYTE Type = p[q];
			const WORD Es = static_cast<WORD>(((p[q + 1] & 0x1F) << 8) | p[q + 2]);
			const size_t DescLen = ((p[q + 3] & 0x0F) << 8) | p[q + 4];

			if ((Type == 0x02 || Type == 0x01) && Out.Video == 0)
				Out.Video = Es;

			if (Type == 0x06) {
				//	component_tag で字幕と文字スーパーを分ける。
				//	文字スーパー (0x38-) は緊急情報等で性格が違う為含めない
				bool fCaption = false;
				size_t d = q + 5;
				while (d + 2 <= q + 5 + DescLen && d + 2 <= End) {
					if (p[d] == 0x52 && p[d + 1] >= 1)
						fCaption = (p[d + 2] >= 0x30 && p[d + 2] <= 0x37);
					d += 2 + p[d + 1];
				}
				if (fCaption) {
					bool fKnown = false;
					for (WORD x : Out.Caption)
						fKnown = fKnown || (x == Es);
					if (!fKnown)
						Out.Caption.push_back(Es);
				}
			}
			q += 5 + DescLen;
		}
		if (!Out.Caption.empty() && Out.Video != 0)
			break;
	}
	return Out;
}

//	映像の開始 PTS。**最初のシーケンスヘッダを含む PES の PTS** を使う
//	(m2v が PTS を捨てる為、音声側と同じ求め方)
int64_t FindVideoStartPts(const std::vector<BYTE> &Ts, WORD VideoPid)
{
	std::vector<BYTE> Es;
	int64_t Pts = -1;

	for (size_t i = 0; i + TS_PACKET_SIZE <= Ts.size(); i += TS_PACKET_SIZE) {
		const BYTE *p = &Ts[i];
		if (p[0] != 0x47 || (p[3] & 0x10) == 0)
			continue;
		if (((p[1] & 0x1F) << 8 | p[2]) != VideoPid)
			continue;

		size_t o = 4;
		if (p[3] & 0x20)
			o += 1 + p[4];
		if (o >= TS_PACKET_SIZE)
			continue;

		if (p[1] & 0x40) {
			const int64_t t = PtsOf(p + o, TS_PACKET_SIZE - o);
			if (t >= 0)
				Pts = t;
			Es.clear();
			const size_t Hdr = (TS_PACKET_SIZE - o >= 9) ? 9 + p[o + 8] : 0;
			if (o + Hdr < TS_PACKET_SIZE)
				Es.assign(p + o + Hdr, p + TS_PACKET_SIZE);
		} else if (!Es.empty()) {
			Es.insert(Es.end(), p + o, p + TS_PACKET_SIZE);
		}

		//	シーケンスヘッダ (00 00 01 B3) を含む PES の PTS が映像の先頭
		for (size_t k = 0; k + 4 <= Es.size(); k++) {
			if (Es[k] == 0 && Es[k + 1] == 0 && Es[k + 2] == 1 && Es[k + 3] == 0xB3)
				return Pts;
		}
		if (Es.size() > 64 * 1024)
			Es.clear();
	}
	return -1;
}

}	// namespace


CTSCaptionSource::CTSCaptionSource() {}
CTSCaptionSource::~CTSCaptionSource() {}


bool CTSCaptionSource::Open(const char *pszSharedName,
							const AribToAviUtl2Options &Options)
{
	m_Captions.clear();
	m_GlyphCount = 0;
	m_MissingGlyphs = 0;
	m_CachedGlyphs = 0;
	m_StreamGlyphs = 0;
	m_NewGlyphs = 0;
	m_ReplacedGlyphs = 0;
	m_StoreFullGlyphs = 0;
	m_UnknownMd5.clear();
	m_GlyphReport.clear();
	m_szError[0] = L'\0';

	std::vector<BYTE> Ts;
	if (!ReadSharedMemory(pszSharedName, &Ts)) {
		::lstrcpynW(m_szError, L"共有メモリを開けません", 128);
		return false;
	}

	const Pids pids = FindPids(Ts);
	if (pids.Caption.empty()) {
		::lstrcpynW(m_szError, L"字幕のストリームが見つかりません", 128);
		return false;
	}

	const int64_t VideoStart =
		pids.Video != 0 ? FindVideoStartPts(Ts, pids.Video) : -1;

	//	--- 字幕の PES を組み立てて解く --------------------------------------
	struct Pending { std::vector<BYTE> Pes; };
	std::map<WORD, Pending> Bufs;
	std::vector<int> DrcsCodes;
	std::map<int, TSMemoryDrcsGlyph> Glyphs;		// 符号 -> 字形
	//	真の間は字形の定義だけを拾い、字幕文は捨てる
	bool fGlyphsOnly = false;

	auto Handle = [&](const std::vector<BYTE> &Pes) {
		if (Pes.size() < 9 || Pes[0] || Pes[1] || Pes[2] != 0x01)
			return;
		const int64_t Pts = PtsOf(Pes.data(), Pes.size());
		const size_t HeaderLen = Pes[8];
		if (9 + HeaderLen + 3 > Pes.size())
			return;

		const BYTE *pl = &Pes[9 + HeaderLen];
		const size_t PlSize = Pes.size() - 9 - HeaderLen;
		const size_t Skip = 3 + (pl[2] & 0x0F);
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

		if (Id == 0x00 || Id == 0x20) {
			if (Tmd == 0x02) q += 5;
			if (q >= Size) return;
			const int Langs = body[q++];
			for (int i = 0; i < Langs && q < Size; i++) {
				const int Dmf = body[q++] & 0x0F;
				if ((Dmf >> 2) == 0x03) q++;
				q += 4;
			}
		} else {
			if (Tmd == 0x01 || Tmd == 0x02) q += 5;
		}

		if (q + 3 > Size) return;
		const size_t Loop = (body[q] << 16) | (body[q + 1] << 8) | body[q + 2];
		q += 3;
		const size_t End = q + Loop < Size ? q + Loop : Size;

		while (q + 5 <= End) {
			if (body[q] != 0x1F) return;
			const BYTE Param = body[q + 1];
			const size_t Len = (body[q + 2] << 16) | (body[q + 3] << 8) | body[q + 4];
			if (q + 5 + Len > End) return;
			const BYTE *pUnit = body + q + 5;

			if (Param == 0x20 && !fGlyphsOnly) {	// 本文
				std::vector<AribItem> Items;
				AribDecodeText(pUnit, Len, &Items);
				AribCaptionLayout Layout;
				AribItemsToAviUtl2(Items, Options, &DrcsCodes, &Layout);

				//	**行ごとに 1 件にする。**放送は行ごとに座標を
				//	持っているので、まとめると位置も背景も合わなくなる。
				//	本文の無い物 (画面消去だけ等) は Lines が空になる
				const double Seconds = (Pts >= 0 && VideoStart >= 0)
									   ? PtsDiffSeconds(VideoStart, Pts) : -1.0;

				//	**ここで前の字幕が消える。**放送は
				//	  ・画面消去 (CS) だけのデータユニット
				//	  ・次の字幕 (書く前に画面を消す)
				//	の形で「消す時刻」を送って来る。拾わないと
				//	次の字幕が来るまで出しっぱなしになる
				//	(実測: 17 組中 1 組、最大 2.4 秒長く出ていた)。
				//
				//	**本文が空でも CS が無ければ消さない。**属性を
				//	変えただけのデータユニットで消えてしまう為
				bool fClear = false;
				for (const AribItem &it : Items)
					fClear = fClear || (it.Type == AribItemType::ClearScreen);
				if (Seconds >= 0.0 && (fClear || !Layout.Lines.empty())) {
					for (TSMemoryCaption &Prev : m_Captions) {
						if (Prev.EndSeconds < 0.0 && Prev.Seconds < Seconds)
							Prev.EndSeconds = Seconds;
					}
				}

				for (const AribCaptionLine &Line : Layout.Lines) {
					TSMemoryCaption c;
					c.Text = Line.Text;
					c.Left = Line.Left;
					c.Top = Line.Top;
					c.PlaneWidth = Layout.PlaneWidth;
					c.PlaneHeight = Layout.PlaneHeight;
					c.Seconds = Seconds;
					m_Captions.push_back(c);
				}
			} else if (Param == 0x30 || Param == 0x31) {	// 外字
				//	字形の定義。同じ符号は後から来た物で上書きする
				if (Len >= 4) {
					size_t k = 1;
					const int Count = pUnit[0];
					for (int n = 0; n < Count && k + 3 <= Len; n++) {
						//	**定義の符号は本文の符号と形が違う。**
						//	定義側は 2 バイトの character_code で、
						//	1 バイト外字 (パラメータ 0x30) の場合は
						//	  上位 … どの外字集合か
						//	  下位 … 集合の中の符号 (本文で使う値)
						//	になっている。そのまま 2 バイトを鍵にすると
						//	**本文の 0x21 と定義の 0x0121 が結び付かず、
						//	字形を受け取っていても必ず「無し」になる**
						//	(実機で 《 》 が出なかった原因)。
						//	libaribcaption も 1 バイトは下位 7 ビット、
						//	2 バイトは 0x7F7F でマスクしている
						const int Raw = (pUnit[k] << 8) | pUnit[k + 1];
						const int Code = (Param == 0x30)
										 ? (Raw & 0x7F)
										 : ((Raw >= 0xEC00 && Raw <= 0xF8FF)
											? Raw : (Raw & 0x7F7F));
						k += 2;
						const int Fonts = pUnit[k++];
						for (int f = 0; f < Fonts && k < Len; f++) {
							const int Mode = pUnit[k++] & 0x0F;
							if (Mode > 1 || k + 3 > Len)
								break;
							TSMemoryDrcsGlyph g;
							g.Depth = pUnit[k];
							g.Width = pUnit[k + 1];
							g.Height = pUnit[k + 2];
							k += 3;
							int Bits = 1;
							while ((1 << Bits) < g.Depth + 2)
								Bits++;
							const size_t Bytes =
								(static_cast<size_t>(g.Width) * g.Height * Bits + 7) / 8;
							if (k + Bytes > Len)
								break;
							g.Pattern.assign(pUnit + k, pUnit + k + Bytes);
							k += Bytes;
							Glyphs[Code] = g;
						}
					}
				}
			}
			q += 5 + Len;
		}
	};

	auto Scan = [&](const std::vector<BYTE> &Data) {
		Bufs.clear();
		for (size_t i = 0; i + TS_PACKET_SIZE <= Data.size(); i += TS_PACKET_SIZE) {
			const BYTE *p = &Data[i];
			if (p[0] != 0x47 || (p[3] & 0x10) == 0)
				continue;
			const WORD Pid = static_cast<WORD>(((p[1] & 0x1F) << 8) | p[2]);
			bool fTarget = false;
			for (WORD x : pids.Caption)
				fTarget = fTarget || (x == Pid);
			if (!fTarget)
				continue;

			size_t o = 4;
			if (p[3] & 0x20)
				o += 1 + p[4];
			if (o >= TS_PACKET_SIZE)
				continue;

			if (p[1] & 0x40) {
				auto it = Bufs.find(Pid);
				if (it != Bufs.end() && !it->second.Pes.empty())
					Handle(it->second.Pes);
				Bufs[Pid].Pes.assign(p + o, p + TS_PACKET_SIZE);
			} else if (Bufs.count(Pid)) {
				Bufs[Pid].Pes.insert(Bufs[Pid].Pes.end(),
									 p + o, p + TS_PACKET_SIZE);
			}
		}
		for (auto &e : Bufs) {
			if (!e.second.Pes.empty())
				Handle(e.second.Pes);
		}
	};

	//	**先に字幕だけの共有メモリから字形を拾う。**
	//	TVTest 側は字幕のパケットを別に長く溜めている
	//	(字幕は取り込む量の 0.01〜0.02% しか無いので安く持てる)。
	//	AviUtl2 側が見えるのは取り込んだ瞬間の窓 (8〜14 秒) だけなので、
	//	**字形の定義 (再送間隔は中央値 14.2 秒、最大 210 秒) は
	//	そちらでないと揃わない**。
	//
	//	**本文は取らない。**こちらは古いパケットも含むので、
	//	字幕文まで拾うと重複したり時刻が合わなくなる。
	//	先に流して後から本編で上書きさせ、新しい字形を優先する
	std::set<int> StreamCodes;		// 蓄積から拾えた符号 (切り分け用)
	{
		std::vector<BYTE> Extra;
		char szExtra[MAX_PATH];
		std::snprintf(szExtra, sizeof(szExtra), "%s.caption", pszSharedName);
		if (ReadSharedMemory(szExtra, &Extra)) {
			fGlyphsOnly = true;
			Scan(Extra);
			fGlyphsOnly = false;
			m_StreamGlyphs = static_cast<int>(Glyphs.size());
			for (const auto &e : Glyphs)
				StreamCodes.insert(e.first);
		}
	}

	Scan(Ts);

	//	--- 受け取った字形を覚えておく ---------------------------------------
	const WORD CaptionPid = pids.Caption.empty() ? 0 : pids.Caption[0];
	const DWORD Now = ::GetTickCount();
	if (Options.UseGlyphCache && CaptionPid != 0) {
		//	**溜まり過ぎたら丸ごと捨てる。**外字の符号は 1 つの文字集合で
		//	94 個までなので、普通はここに掛からない
		if (g_GlyphCache.size() > GLYPH_CACHE_MAX)
			g_GlyphCache.clear();
		for (const auto &e : Glyphs) {
			CachedGlyph c;
			c.Glyph = e.second;
			c.Tick = Now;
			g_GlyphCache[GlyphKey(pids.Service, CaptionPid, e.first)] = c;
		}
	}

	//	**どの符号をどこから拾ったかを残す。**
	//	外字が化けた時、原因が「窓に入らなかった」のか
	//	「そもそも定義が流れていない」のかをログで切り分ける為
	{
		WCHAR sz[128];
		::StringCchPrintfW(sz, ARRAYSIZE(sz),
						   L"サービス %04X / 字幕 PID %04X / 蓄積 %d 字形 / "
						   L"キャッシュ %d 件 :",
						   pids.Service, CaptionPid, m_StreamGlyphs,
						   static_cast<int>(g_GlyphCache.size()));
		m_GlyphReport = sz;
	}
	auto Report = [&](int Code, LPCWSTR pszFrom) {
		WCHAR sz[64];
		::StringCchPrintfW(sz, ARRAYSIZE(sz), L" %04X=%s", Code, pszFrom);
		m_GlyphReport += sz;
	};

	//	字形が無かった外字の添字。**代替文字に置き換える為に要る。**
	//	変換はフォントを組み立てる前に済んでいるので、その時点では
	//	字形が揃うかどうかが判らない
	std::set<size_t> MissingIndex;
	//	参照の添字 -> 私用領域の枠 (今のフォントに入っている物だけ)
	std::map<size_t, int> SlotOf;
	//	参照の添字 -> 置き換える本物の文字 (対応表で引けた物)
	std::map<size_t, std::wstring> TextOf;

	//	--- 外字をどう出すかを決める -----------------------------------------
	//	**上から順に試す。**
	//	  1. 対応表で本物の文字に置き換える → その回から出る
	//	  2. 貯め込みのフォントに既に入っている → その回から出る
	//	  3. 初めて見る字形 → 貯めて、次の起動から出す。今回は代替文字
	//
	//	1 が効くのは、放送の外字の多くが Unicode に実在する文字を
	//	ビットマップで送っているだけだから (drcs_replace.h を参照)
	if (!DrcsCodes.empty() && !Options.DrcsFont.empty()) {
		auto Place = [&](size_t i, const TSMemoryDrcsGlyph &g) {
			std::wstring Text, Md5;
			if (TSMemoryDrcsReplaceFind(g, &Text, &Md5)) {
				TextOf[i] = Text;
				m_ReplacedGlyphs++;
				return;
			}

			//	**引けなかった md5 を控えておく。**
			//	利用者が TSMemoryDrcsMap.txt に足せるようにする為
			if (!Md5.empty() && m_UnknownMd5.find(Md5) == std::wstring::npos) {
				if (!m_UnknownMd5.empty())
					m_UnknownMd5 += L" ";
				m_UnknownMd5 += Md5;
			}

			//	**枠は字形の中身で決まる。**ARIB の符号は番組ごとに
			//	意味が変わるので、そのまま私用領域に写すと別の番組で
			//	違う字が出る。中身で引けば取り違えない
			const int Slot = TSMemoryDrcsStoreAdd(g);
			if (Slot >= 0 && Slot < TSMemoryDrcsStoreLoadedCount()) {
				SlotOf[i] = Slot;			// 今のフォントに入っている
				return;
			}
			MissingIndex.insert(i);
			if (Slot < 0)
				m_StoreFullGlyphs++;		// 貯め込みが一杯。再起動しても出ない
			else
				m_NewGlyphs++;				// 貯めた。次の起動から出る
		};

		for (size_t i = 0; i < DrcsCodes.size(); i++) {
			auto it = Glyphs.find(DrcsCodes[i]);
			if (it == Glyphs.end()) {
				//	**字形が届かなかった外字。**
				//	リングバッファの窓より前に定義されている場合に起こる。
				//	前の取り込みで受け取っていれば、そちらを使う
				bool fCached = false;
				if (Options.UseGlyphCache && CaptionPid != 0) {
					auto c = g_GlyphCache.find(
						GlyphKey(pids.Service, CaptionPid, DrcsCodes[i]));
					if (c != g_GlyphCache.end()
							&& Now - c->second.Tick <= GLYPH_CACHE_LIFE_MS) {
						m_CachedGlyphs++;
						fCached = true;
						Report(DrcsCodes[i], L"キャッシュ");
						Place(i, c->second.Glyph);
					}
				}
				if (!fCached) {
					m_MissingGlyphs++;
					MissingIndex.insert(i);
					Report(DrcsCodes[i], L"無し");
				}
				continue;
			}
			Report(DrcsCodes[i],
				   StreamCodes.count(DrcsCodes[i]) ? L"蓄積" : L"本編");
			Place(i, it->second);
		}
		m_GlyphCount = SlotOf.size() + TextOf.size();

		//	**変換の時点では枠が判らない。**
		//	字形は変換の後に揃うので、本文には「参照の順番」で仮の
		//	私用領域の文字が入っている。ここで本当の枠に付け替え、
		//	フォントに無い物は代替文字にする。
		//	付け替えないと**フォントに無い字を指して豆腐 (□) になる**
		//	**1 回の走査で書き換える。**順に置換すると、付け替えた先の
		//	番号を次の回でまた拾ってしまう (0 番 -> 2 番、2 番 -> 0 番 の
		//	ような入れ替えで壊れる)
		const std::wstring Prefix = L"<@" + Options.DrcsFont + L">";
		const std::wstring Suffix = L"<@>";
		const size_t Unit = Prefix.size() + 1 + Suffix.size();

		for (TSMemoryCaption &c : m_Captions) {
			std::wstring Out;
			Out.reserve(c.Text.size());
			size_t p = 0;
			while (p < c.Text.size()) {
				const size_t q = c.Text.find(Prefix, p);
				if (q == std::wstring::npos || q + Unit > c.Text.size()) {
					Out.append(c.Text, p, std::wstring::npos);
					break;
				}
				const size_t n = static_cast<size_t>(
					c.Text[q + Prefix.size()] - Options.DrcsFirstCode);
				if (n >= DrcsCodes.size()
						|| c.Text.compare(q + Prefix.size() + 1,
										  Suffix.size(), Suffix) != 0) {
					//	外字の書き方ではない。そのまま送る
					Out.append(c.Text, p, q + Prefix.size() - p);
					p = q + Prefix.size();
					continue;
				}

				Out.append(c.Text, p, q - p);
				auto t = TextOf.find(n);
				auto s = SlotOf.find(n);
				if (t != TextOf.end()) {
					//	**本物の文字なのでフォントを切り替えない。**
					//	本文の書体のまま出て、編集も検索もできる
					Out += t->second;
				} else if (s != SlotOf.end()) {
					Out += Prefix;
					Out += static_cast<wchar_t>(Options.DrcsFirstCode + s->second);
					Out += Suffix;
				} else {
					Out += Options.DrcsFallback;
				}
				p = q + Unit;
			}
			c.Text.swap(Out);
		}
	}

	//	**足した字形があれば貯め込みを書き直す。**
	//	フォントに入るのは次の起動時 (drcs_store.h 参照)
	TSMemoryDrcsStoreFlush();

	return !m_Captions.empty();
}
