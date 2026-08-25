/*
 * 共有メモリ読み出し層 (shared_memory.c / multi_file.c) の 64bit 動作確認。
 *
 * TSMemory.tvtp が作るのと同じ形の名前付き共有メモリを用意し、
 * リングバッファの線形化・シーク・読み出しが期待通りかを確認する。
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "shared_memory.h"
#include "multi_file.h"

#define SHM_NAME	"tsmemtest.tvtv"
#define PACKET_SIZE	188
#define PACKET_NUM	16

static int g_failures = 0;

static void check(const char *what, int ok)
{
	printf("%-52s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		g_failures++;
}

/* リングバッファの i 番目のパケットの中身 (先頭バイトで判別する) */
static unsigned char expected_byte(int index)
{
	return (unsigned char)(0x40 + index);
}

int main(void)
{
	SECURITY_DESCRIPTOR sd;
	SECURITY_ATTRIBUTES sa;
	HANDLE mutex, map;
	unsigned char *view;
	DWORD *info;
	const DWORD data_size = PACKET_SIZE * PACKET_NUM;
	const DWORD map_size = sizeof(DWORD) * 4 + data_size;
	/* 先頭が 5 番目のパケットになるように Pos をずらして折り返しを作る */
	const DWORD start_packet = 5;
	int i;
	intptr_t id;
	MULTI_FILE *mf;
	unsigned char buf[PACKET_SIZE];
	char path[MAX_PATH];

	memset(&sd, 0, sizeof(sd));
	InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
	SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
	memset(&sa, 0, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.lpSecurityDescriptor = &sd;

	mutex = CreateMutexA(&sa, FALSE, SHM_NAME ".mutex");
	map = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, map_size, SHM_NAME);
	if (mutex == NULL || map == NULL) {
		printf("failed to create shared memory\n");
		return 1;
	}
	view = (unsigned char *)MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 0);
	if (view == NULL) {
		printf("failed to map view\n");
		return 1;
	}

	info = (DWORD *)view;
	info[0] = data_size;					/* Size */
	info[1] = data_size;					/* Used */
	info[2] = start_packet * PACKET_SIZE;	/* Pos  */
	info[3] = 0;							/* Reserved */

	/* 論理的な i 番目のパケットを、物理的には (start+i)%N の位置に置く */
	for (i = 0; i < PACKET_NUM; i++) {
		const DWORD phys = (start_packet + i) % PACKET_NUM;
		unsigned char *p = view + sizeof(DWORD) * 4 + phys * PACKET_SIZE;
		memset(p, expected_byte(i), PACKET_SIZE);
	}

	/* パスの中からファイル名部分だけが共有メモリ名として使われる */
	strcpy(path, "C:\\somewhere\\that\\does\\not\\exist\\" SHM_NAME);

	/* --- shared_memory.c ------------------------------------------------ */
	id = open_shared_memory(path);
	check("open_shared_memory() returns a handle", id > 0);
	if (id <= 0)
		return 1;

	check("shm_seek(END) == total size",
		  shm_seek(id, 0, SEEK_END) == (__int64)data_size);
	check("shm_seek(SET,0) == 0", shm_seek(id, 0, SEEK_SET) == 0);

	{
		int ok = 1;
		for (i = 0; i < PACKET_NUM; i++) {
			if (shm_read(id, buf, PACKET_SIZE) != PACKET_SIZE) {
				ok = 0;
				break;
			}
			if (buf[0] != expected_byte(i) || buf[PACKET_SIZE - 1] != expected_byte(i)) {
				ok = 0;
				break;
			}
		}
		check("ring buffer is linearized in the right order", ok);
	}

	check("shm_tell() at end == total size", shm_tell(id) == (__int64)data_size);
	check("shm_read() past the end returns 0", shm_read(id, buf, PACKET_SIZE) == 0);

	/* 途中シークして読み直す */
	check("shm_seek(SET, 3 packets)",
		  shm_seek(id, PACKET_SIZE * 3, SEEK_SET) == PACKET_SIZE * 3);
	check("read after seek gives packet #3",
		  shm_read(id, buf, PACKET_SIZE) == PACKET_SIZE && buf[0] == expected_byte(3));

	check("shm_close()", shm_close(id) == 1);

	/* --- multi_file.c --------------------------------------------------- */
	mf = open_multi_file(path);
	check("open_multi_file() returns a MULTI_FILE", mf != NULL);
	if (mf != NULL) {
		check("multi_file seek(END) == total size",
			  mf->seek(mf, 0, SEEK_END) == (__int64)data_size);
		mf->seek(mf, 0, SEEK_SET);
		check("multi_file read gives packet #0",
			  mf->read(mf, buf, PACKET_SIZE) == PACKET_SIZE && buf[0] == expected_byte(0));
		mf->close(mf);
	}

	/* --- 共有メモリが無い場合 ------------------------------------------- */
	check("open_shared_memory() fails for an unknown name",
		  open_shared_memory("C:\\nowhere\\no_such_shared_memory.tvtv") == -1);

	UnmapViewOfFile(view);
	CloseHandle(map);
	CloseHandle(mutex);

	printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
		   g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
