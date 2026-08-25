//----------------------------------------------------------------------------
//	TS の映像 PES から PTS を拾う (テスト用の簡易実装)
//
//	「取り込んだ内容が元動画のどのあたりなのか」「どれだけ欠けたのか」を
//	デコードせずに調べる為の物。映像の PES ヘッダ (0x000001E0〜0xEF) の
//	PTS を読むだけで、PMT は見ない。
//----------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <cstdint>
#include <cstddef>

constexpr int64_t PTS_NONE = -1;
constexpr int64_t PTS_WRAP = 1LL << 33;

//	1 パケットから映像の PTS を取り出す (無ければ PTS_NONE)
inline int64_t GetPacketVideoPts(const BYTE *p)
{
	if (p[0] != 0x47)
		return PTS_NONE;
	if ((p[1] & 0x40) == 0)			// payload_unit_start_indicator
		return PTS_NONE;
	if ((p[3] & 0x10) == 0)			// payload なし
		return PTS_NONE;

	int Offset = 4;
	if (p[3] & 0x20)				// adaptation_field あり
		Offset += 1 + p[4];
	if (Offset + 14 > 188)
		return PTS_NONE;

	const BYTE *q = p + Offset;
	if (q[0] != 0x00 || q[1] != 0x00 || q[2] != 0x01)
		return PTS_NONE;
	if (q[3] < 0xE0 || q[3] > 0xEF)	// 映像の stream_id
		return PTS_NONE;
	if ((q[7] & 0x80) == 0)			// PTS なし
		return PTS_NONE;

	return (static_cast<int64_t>(q[9] & 0x0E) << 29)
		| (static_cast<int64_t>(q[10]) << 22)
		| (static_cast<int64_t>(q[11] & 0xFE) << 14)
		| (static_cast<int64_t>(q[12]) << 7)
		| (static_cast<int64_t>(q[13]) >> 1);
}

//	範囲内の最初と最後の映像 PTS を得る
inline bool ScanVideoPts(const BYTE *pData, size_t Size, int64_t *pFirst, int64_t *pLast)
{
	*pFirst = *pLast = PTS_NONE;

	for (size_t i = 0; i + 188 <= Size; i += 188) {
		const int64_t Pts = GetPacketVideoPts(pData + i);
		if (Pts == PTS_NONE)
			continue;
		if (*pFirst == PTS_NONE)
			*pFirst = Pts;
		*pLast = Pts;
	}

	return *pFirst != PTS_NONE;
}

//	PTS の差を秒で返す (33bit の巻き戻りを考慮)
inline double PtsDiffSeconds(int64_t From, int64_t To)
{
	int64_t d = To - From;
	if (d < -(PTS_WRAP / 2))
		d += PTS_WRAP;
	else if (d > PTS_WRAP / 2)
		d -= PTS_WRAP;
	return static_cast<double>(d) / 90000.0;
}
