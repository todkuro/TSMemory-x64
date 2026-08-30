//----------------------------------------------------------------------------
//	外字 (DRCS) をフォントとして AviUtl2 に渡す
//
//	ARIB 字幕の外字は、字形のビットマップが放送に乗って来る。
//	それをそのままフォントに仕立てて AviUtl2 に登録出来れば、
//	字幕は**テキストのまま**放送どおりの字形で表示出来る。
//
//	  ・画像オブジェクトを混ぜずに済む (編集出来る)
//	  ・本文の書体は利用者のプリセットのまま、外字だけ <@...> で切り替える
//	  ・システムへのフォント導入が要らない (メモリ上で完結)
//
//	`AddFontResourceEx(FR_PRIVATE)` では **DirectWrite から見えない**
//	(実測済み) 為、AviUtl2 の register_font_collection() を使う。
//----------------------------------------------------------------------------
#pragma once

#include <windows.h>

struct HOST_APP_TABLE;
struct EDIT_HANDLE;

//	本体のテーブルを渡す (RegisterPlugin から呼ぶ)
void TSMemoryFontSetHost(HOST_APP_TABLE *host, EDIT_HANDLE *edit);

//	**フォントを登録出来る時期か。**
//
//	AviUtl2 は初期化 (RegisterPlugin) の中でしか受け付けない。
//	取り込みの時に呼ぶと本体が例外を投げ、**AviUtl2 ごと落ちる**
//	(実機のログ: not register except initialize in
//	 Plugin::CommonPluginService::registerFontCollection())。
bool TSMemoryFontCanRegister();

//	初期化の始まり / 終わりを伝える (RegisterPlugin から呼ぶ)
void TSMemoryFontEndInitialize();

//	フォントデータ (TTF) を AviUtl2 に登録する。
//
//	pData/Size … メモリ上のフォント。呼び出し後も保持し続ける必要がある
//	             (DirectWrite が参照する)。この関数が複製を持つ。
//	戻り値     … 登録出来たら true
bool TSMemoryRegisterFontCollection(const BYTE *pData, size_t Size);

//	登録したフォントが AviUtl2 側から見えるかを確かめてログに出す。
//	pszFamily … 期待するフォント名
void TSMemoryVerifyFont(LPCWSTR pszFamily);

//	検証用: フォントファイルを読み、登録して、見えるかを確かめる
void TSMemoryFontProbe(LPCWSTR pszPath);
