//----------------------------------------------------------------------------
//	外字 (DRCS) の字形を貯めておく
//
//	**フォントは初期化の時にしか渡せない。**AviUtl2 の
//	register_font_collection() は RegisterPlugin の中でしか受け付けず、
//	取り込みの時に呼ぶと本体ごと落ちる (実機で確認)。
//	一方、外字の字形は取り込むまで判らない。
//
//	その為、受け取った字形をファイルに貯めておき、**次の起動の初期化で**
//	まとめて登録する。
//
//	  1. 受け取った字形をここに貯める (TSMemoryDRCS.dat に残す)
//	  2. 次の起動の RegisterPlugin で、貯めた字形から TTF を組み立て、
//	     register_font_collection() でメモリのまま渡す
//
//	**ファイルはプラグインの ini と同じ場所にしか置かない。**
//	`ProgramData\aviutl2\Font` に TTF を置く方法でも動くが
//	(本体が起動時に読む)、システム側のフォルダを触らずに済むので
//	こちらにしている。次の起動から使える点はどちらも変わらない。
//
//	**鍵は ARIB の符号ではなく字形そのもの。**符号 (0x21 から順) の
//	意味は番組ごとに変わるので、符号を鍵にすると別の番組・別の
//	チャンネルで違う字が出てしまう。中身で引けば取り違えようがない。
//----------------------------------------------------------------------------
#pragma once

#include <windows.h>

#include "drcs_ttf.h"

//	貯めてある字形を読み込む (初期化の時に 1 度だけ)。
//	pszIniFile … プラグインの ini。同じフォルダに貯め込みを置く
void TSMemoryDrcsStoreLoad(LPCWSTR pszIniFile);

//	読み込んだ字形からフォントを組み立てて本体に登録する。
//
//	**RegisterPlugin の中からしか呼べない** (drcs_font.h を参照)。
//	TSMemoryDrcsStoreLoad() と TSMemoryFontSetHost() の後に呼ぶ事。
//	pszFontName … フォントの名前 ([Caption] DrcsFont)
bool TSMemoryDrcsStoreRegisterFont(LPCWSTR pszFontName);

//	**今のフォントに入っている字形の数。**
//	本体が起動時に読み込んだ物と同じ数になる。
//	これより後ろの枠は「次の起動から使える」枠
int TSMemoryDrcsStoreLoadedCount();

//	字形を探す。**中身が同じなら同じ枠**を返す。無ければ -1
int TSMemoryDrcsStoreFind(const TSMemoryDrcsGlyph &Glyph);

//	字形を足して枠の番号を返す。既にあればその番号。
//	上限を超えた時は -1
int TSMemoryDrcsStoreAdd(const TSMemoryDrcsGlyph &Glyph);

//	足した分があれば貯め込みを書き直す。
//	書いたら true (次の起動から増えた字形が使える)
bool TSMemoryDrcsStoreFlush();
