//----------------------------------------------------------------------------
//	MSVC 前提のコードを clang (mingw-w64) でビルドする為の補完定義
//	ビルド時に -include で強制インクルードする
//----------------------------------------------------------------------------
#pragma once

//	MSVC の <windows.h> は C++ でも min/max マクロを定義するが
//	mingw-w64 は C 言語の時しか定義しない
#ifdef __cplusplus
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#endif
