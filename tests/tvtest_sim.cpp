//----------------------------------------------------------------------------
//	TVTest 側を模擬して AviUtl2 に取り込み要求を送る
//
//	  tvtest_sim <ts-file> <出力する .tvtv のフルパス>
//
//	TSMemory.tvtp と同じ手順で
//	  1. TS を共有メモリ (名前 = .tvtv のファイル名) に載せる
//	  2. 0 バイトのダミーファイルを作る
//	  3. TSMemoryBridge.* 経由で読み込み要求を送る
//	を行う。TVTest と受信環境が無くても連携経路を試せる。
//
//	終了時クラッシュの再現・回帰確認に使う (tests/tools/test-exit.sh)。
//	共有メモリは 40 秒保持してから終了する。
//----------------------------------------------------------------------------
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "tsmemory_ipc.h"

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3) { wprintf(L"usage: sender <ts-file> <out.tvtv full path>\n"); return 1; }

    // TS を読む
    FILE *fp = _wfopen(argv[1], L"rb");
    if (!fp) { wprintf(L"cannot open %s\n", argv[1]); return 1; }
    fseek(fp, 0, SEEK_END); long size = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (size > 10*1024*1024) size = 10*1024*1024;      // MemorySize 相当
    std::vector<BYTE> ts((size_t)size);
    ts.resize(fread(ts.data(), 1, ts.size(), fp));
    fclose(fp);

    // 共有メモリ名 = .tvtv のファイル名
    const wchar_t *name = wcsrchr(argv[2], (wchar_t)92);
    name = name ? name + 1 : argv[2];
    char nameA[MAX_PATH]; WideCharToMultiByte(CP_ACP, 0, name, -1, nameA, MAX_PATH, 0, 0);
    char mutexA[MAX_PATH]; wsprintfA(mutexA, "%s.mutex", nameA);

    SECURITY_DESCRIPTOR sd; SECURITY_ATTRIBUTES sa;
    TSMemoryInitSecurityAttributes(&sd, &sa);

    HANDLE hMx = CreateMutexA(&sa, FALSE, mutexA);
    DWORD total = (DWORD)ts.size();
    HANDLE hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0,
                                     sizeof(DWORD)*4 + total, nameA);
    if (!hMap) { wprintf(L"CreateFileMapping failed\n"); return 1; }
    DWORD *p = (DWORD *)MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, 0);
    p[0] = total; p[1] = total; p[2] = 0; p[3] = 0;
    memcpy((BYTE *)p + sizeof(DWORD)*4, ts.data(), total);

    // 0 バイトのダミーファイル
    HANDLE hf = CreateFileW(argv[2], GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, 0, 0);
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);

    // 要求を送る
    HANDLE hPMap = OpenFileMappingW(FILE_MAP_WRITE, FALSE, TSMEMORY_IPC_PARAM_MAP);
    HANDLE hPMx  = OpenMutexW(SYNCHRONIZE, FALSE, TSMEMORY_IPC_PARAM_MUTEX);
    HANDLE hEv   = OpenEventW(EVENT_MODIFY_STATE, FALSE, TSMEMORY_IPC_REQUEST_EVENT);
    if (!hPMap || !hPMx || !hEv) { wprintf(L"AviUtl2 side not listening\n"); return 1; }

    TSMEMORY_REQUEST *pr = (TSMEMORY_REQUEST *)MapViewOfFile(hPMap, FILE_MAP_WRITE, 0, 0, 0);
    WaitForSingleObject(hPMx, 3000);
    pr->Version = TSMEMORY_IPC_VERSION;
    pr->Serial += 1;
    lstrcpynW(pr->FileName, argv[2], MAX_PATH);
    ReleaseMutex(hPMx);
    SetEvent(hEv);

    wprintf(L"request sent: %s  (%u bytes)\n", argv[2], total);
    wprintf(L"holding shared memory for 40 sec...\n");
    Sleep(40000);
    return 0;
}
