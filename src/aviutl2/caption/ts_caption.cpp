//----------------------------------------------------------------------------
//	*.tvtv の字幕 (ts_caption.h を参照)
//----------------------------------------------------------------------------
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "ts_caption.h"
#include "arib_text.h"

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
};

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
	m_Font.clear();
	m_GlyphCount = 0;
	m_MissingGlyphs = 0;
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

			if (Param == 0x20) {			// 本文
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
					c.CenterX = Line.CenterX();
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
						const int Code = (pUnit[k] << 8) | pUnit[k + 1];
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

	for (size_t i = 0; i + TS_PACKET_SIZE <= Ts.size(); i += TS_PACKET_SIZE) {
		const BYTE *p = &Ts[i];
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
			Bufs[Pid].Pes.insert(Bufs[Pid].Pes.end(), p + o, p + TS_PACKET_SIZE);
		}
	}
	for (auto &e : Bufs) {
		if (!e.second.Pes.empty())
			Handle(e.second.Pes);
	}

	//	--- 外字のフォントを組み立てる ---------------------------------------
	//	割り当てた順 (DrcsCodes) と同じ並びで字形を並べる
	if (!DrcsCodes.empty() && !Options.DrcsFont.empty()) {
		std::vector<TSMemoryDrcsGlyph> List;
		for (size_t i = 0; i < DrcsCodes.size(); i++) {
			auto it = Glyphs.find(DrcsCodes[i]);
			if (it == Glyphs.end()) {
				//	**字形が届かなかった外字。**
				//	リングバッファの窓より前に定義されている場合に起こる
				m_MissingGlyphs++;
				continue;
			}
			TSMemoryDrcsGlyph g = it->second;
			g.Code = static_cast<wchar_t>(Options.DrcsFirstCode + i);
			List.push_back(g);
		}
		if (!List.empty()) {
			m_GlyphCount = List.size();
			TSMemoryBuildDrcsFont(List, Options.DrcsFont.c_str(), &m_Font);
		}
	}

	return !m_Captions.empty();
}
