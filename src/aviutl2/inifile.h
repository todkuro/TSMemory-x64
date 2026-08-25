#pragma once

//	設定ファイルから文字列を読む。
//
//	GetPrivateProfileStringW() は BOM の無いファイルを ANSI (日本語環境なら
//	CP932) として読む為、UTF-8 で書かれた TSMemory-TVTestSrc.ini から日本語の値を
//	読むと文字化けする。プリセット名や保存先のパスに日本語が入る為、
//	UTF-8 のファイルは自分で読む。
//
//	・UTF-16 の BOM がある      -> GetPrivateProfileStringW() に任せる
//	・UTF-8 として解釈出来る    -> 自前で読む (BOM の有無は問わない)
//	・それ以外 (CP932 など)     -> GetPrivateProfileStringW() に任せる
//
//	戻り値は out に格納した文字数 (終端を除く)。
DWORD TSMemoryGetIniString(LPCWSTR ini, LPCWSTR section, LPCWSTR key, LPCWSTR def,
						   LPWSTR out, DWORD size);
