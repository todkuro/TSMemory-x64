#pragma once


#include <vector>
#include "MediaDecoder.h"
#include "TsStream.h"
#include "TsTable.h"
#include "TsUtilClass.h"


class CTsSelector : public CMediaDecoder
{
public:
	CTsSelector(IEventHandler *pEventHandler = NULL);
	virtual ~CTsSelector();

// CMediaDecoder
	virtual void Reset(void);
	virtual const bool InputMedia(CMediaData *pMediaData, const DWORD dwInputIndex = 0UL);

// CTsSelector
	enum {
		STREAM_MPEG1VIDEO		= 0x00000001UL,
		STREAM_MPEG2VIDEO		= 0x00000002UL,
		STREAM_SUBTITLE			= 0x00000004UL,
		STREAM_DATACARROUSEL	= 0x00000008UL,
		STREAM_AAC				= 0x00000010UL,
		STREAM_H264				= 0x00000020UL,
		//	H.265/HEVC (stream_type 0x24)。
		//	本リポジトリでの追加。BonTsEngine は本家 TVTest から
		//	2017-09-30 に削除されており上流に戻す先が無い為、
		//	ここで直接足している (docs/development.md を参照)。
		//	値は TsSelector.cpp の StreamTypeList[] の並び順に対応する
		STREAM_H265				= 0x00000040UL,
		//	新 4K8K 衛星放送の音声。地上波/BS の AAC (0x0F, ADTS) とは
		//	同期層が違う (LATM/LOAS)。これも本リポジトリでの追加
		STREAM_AAC_LATM			= 0x00000080UL,	// stream_type 0x11
		STREAM_MPEG4_AUDIO		= 0x00000100UL,	// stream_type 0x1C
		STREAM_ALL				= 0xFFFFFFFFUL
	};
	bool SetTargetServiceID(WORD ServiceID=0, DWORD Stream=STREAM_ALL);
	ULONGLONG GetInputPacketCount() const;
	ULONGLONG GetOutputPacketCount() const;

protected:
	bool IsTargetPID(WORD PID) const;
	int GetServiceIndexByID(WORD ServiceID) const;
	bool MakePat(const CTsPacket *pSrcPacket, CTsPacket *pDstPacket);

	static void CALLBACK OnPatUpdated(const WORD wPID, CTsPidMapTarget *pMapTarget, CTsPidMapManager *pMapManager, const PVOID pParam);
	static void CALLBACK OnCatUpdated(const WORD wPID, CTsPidMapTarget *pMapTarget, CTsPidMapManager *pMapManager, const PVOID pParam);
	static void CALLBACK OnPmtUpdated(const WORD wPID, CTsPidMapTarget *pMapTarget, CTsPidMapManager *pMapManager, const PVOID pParam);

	CTsPidMapManager m_PidMapManager;

	WORD m_TargetServiceID;
	WORD m_TargetPmtPID;
	WORD m_TargetEmmPID;
	DWORD m_TargetStream;

	struct TAG_PMTPIDINFO {
		WORD ServiceID;
		WORD PmtPID;
		WORD PcrPID;
		WORD EcmPID;
		std::vector<WORD> EsPIDs;
	};
	std::vector<TAG_PMTPIDINFO> m_PmtPIDList;

	ULONGLONG m_InputPacketCount;
	ULONGLONG m_OutputPacketCount;

	CTsPacket m_PatPacket;
	WORD m_LastTSID;
	WORD m_LastPmtPID;
	BYTE m_LastVersion;
	BYTE m_Version;
};
