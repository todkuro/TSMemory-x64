/*
 * simd_stub.c - 32bit x86 アセンブラ版 SIMD ルーチンのダミー実装
 *
 * TVTestSrc (MPEG-2 VIDEO VFAPI Plug-In) が元々持っていた MMX/SSE/SSE2 の
 * ルーチンは 32bit の x86 アセンブラ (*.asm) と MSVC のインラインアセンブラで
 * 書かれており、x64 ではビルド出来ない。
 *
 * 64bit 版では registry.c の get_simd_mode() が常に 0 を返すようにしてあり、
 * mpeg_video.c の関数テーブル設定ではこれらが選ばれる事はない。
 * 呼び出し側のコードは残っている為、リンクを通すためだけのダミー定義。
 * (万一呼ばれた場合は OutputDebugString で判るようにしてある)
 *
 * ※ これは「x64 では SIMD を使わない」という意味ではない。
 *   x64 用の SIMD は 32bit アセンブラを移植するのではなく intrinsics で
 *   書いており、現在は resize.c の component_resize() が SSE2 を使う。
 *   一番重い処理はそちらなので、この足場は x64 では使っていない。
 *   詳細は docs/development.md の「デコード速度の改善」を参照。
 *
 * ※ このファイルは tools/gen_simd_stub.sh で生成される。
 */
#include <windows.h>

static void m2v_simd_unreachable(const char *name)
{
	OutputDebugStringA("TSMemory: SIMD routine called on x64 build: ");
	OutputDebugStringA(name);
	OutputDebugStringA("\n");
}

void add_diff_to_frame_mmx(void) { m2v_simd_unreachable("add_diff_to_frame_mmx"); }
void chroma420i_to_422_mmx(void) { m2v_simd_unreachable("chroma420i_to_422_mmx"); }
void chroma420i_to_422_sse2(void) { m2v_simd_unreachable("chroma420i_to_422_sse2"); }
void chroma420p_to_422_mmx(void) { m2v_simd_unreachable("chroma420p_to_422_mmx"); }
void chroma420p_to_422_sse2(void) { m2v_simd_unreachable("chroma420p_to_422_sse2"); }
void copy_i_block_to_frame_mmx(void) { m2v_simd_unreachable("copy_i_block_to_frame_mmx"); }
void idct_ap922_mmx(void) { m2v_simd_unreachable("idct_ap922_mmx"); }
void idct_ap922_sse(void) { m2v_simd_unreachable("idct_ap922_sse"); }
void idct_ap922_sse2(void) { m2v_simd_unreachable("idct_ap922_sse2"); }
void idct_llm_mmx(void) { m2v_simd_unreachable("idct_llm_mmx"); }
void idct_reference_sse(void) { m2v_simd_unreachable("idct_reference_sse"); }
void prediction_w16_ff_1st_mmx(void) { m2v_simd_unreachable("prediction_w16_ff_1st_mmx"); }
void prediction_w16_ff_2nd_mmx(void) { m2v_simd_unreachable("prediction_w16_ff_2nd_mmx"); }
void prediction_w16_ff_2nd_sse(void) { m2v_simd_unreachable("prediction_w16_ff_2nd_sse"); }
void prediction_w16_ff_2nd_sse2(void) { m2v_simd_unreachable("prediction_w16_ff_2nd_sse2"); }
void prediction_w16_fh_1st_mmx(void) { m2v_simd_unreachable("prediction_w16_fh_1st_mmx"); }
void prediction_w16_fh_1st_sse(void) { m2v_simd_unreachable("prediction_w16_fh_1st_sse"); }
void prediction_w16_fh_1st_sse2(void) { m2v_simd_unreachable("prediction_w16_fh_1st_sse2"); }
void prediction_w16_fh_2nd_mmx(void) { m2v_simd_unreachable("prediction_w16_fh_2nd_mmx"); }
void prediction_w16_fh_2nd_sse(void) { m2v_simd_unreachable("prediction_w16_fh_2nd_sse"); }
void prediction_w16_fh_2nd_sse2(void) { m2v_simd_unreachable("prediction_w16_fh_2nd_sse2"); }
void prediction_w16_hf_1st_mmx(void) { m2v_simd_unreachable("prediction_w16_hf_1st_mmx"); }
void prediction_w16_hf_1st_sse(void) { m2v_simd_unreachable("prediction_w16_hf_1st_sse"); }
void prediction_w16_hf_1st_sse2(void) { m2v_simd_unreachable("prediction_w16_hf_1st_sse2"); }
void prediction_w16_hf_2nd_mmx(void) { m2v_simd_unreachable("prediction_w16_hf_2nd_mmx"); }
void prediction_w16_hf_2nd_sse(void) { m2v_simd_unreachable("prediction_w16_hf_2nd_sse"); }
void prediction_w16_hf_2nd_sse2(void) { m2v_simd_unreachable("prediction_w16_hf_2nd_sse2"); }
void prediction_w16_hh_1st_mmx(void) { m2v_simd_unreachable("prediction_w16_hh_1st_mmx"); }
void prediction_w16_hh_1st_sse2(void) { m2v_simd_unreachable("prediction_w16_hh_1st_sse2"); }
void prediction_w16_hh_2nd_mmx(void) { m2v_simd_unreachable("prediction_w16_hh_2nd_mmx"); }
void prediction_w16_hh_2nd_sse2(void) { m2v_simd_unreachable("prediction_w16_hh_2nd_sse2"); }
void prediction_w8_ff_1st_mmx(void) { m2v_simd_unreachable("prediction_w8_ff_1st_mmx"); }
void prediction_w8_ff_2nd_mmx(void) { m2v_simd_unreachable("prediction_w8_ff_2nd_mmx"); }
void prediction_w8_ff_2nd_sse(void) { m2v_simd_unreachable("prediction_w8_ff_2nd_sse"); }
void prediction_w8_fh_1st_mmx(void) { m2v_simd_unreachable("prediction_w8_fh_1st_mmx"); }
void prediction_w8_fh_1st_sse(void) { m2v_simd_unreachable("prediction_w8_fh_1st_sse"); }
void prediction_w8_fh_2nd_mmx(void) { m2v_simd_unreachable("prediction_w8_fh_2nd_mmx"); }
void prediction_w8_fh_2nd_sse(void) { m2v_simd_unreachable("prediction_w8_fh_2nd_sse"); }
void prediction_w8_hf_1st_mmx(void) { m2v_simd_unreachable("prediction_w8_hf_1st_mmx"); }
void prediction_w8_hf_1st_sse(void) { m2v_simd_unreachable("prediction_w8_hf_1st_sse"); }
void prediction_w8_hf_2nd_mmx(void) { m2v_simd_unreachable("prediction_w8_hf_2nd_mmx"); }
void prediction_w8_hf_2nd_sse(void) { m2v_simd_unreachable("prediction_w8_hf_2nd_sse"); }
void prediction_w8_hh_1st_mmx(void) { m2v_simd_unreachable("prediction_w8_hh_1st_mmx"); }
void prediction_w8_hh_2nd_mmx(void) { m2v_simd_unreachable("prediction_w8_hh_2nd_mmx"); }
void setup_qw_mmx(void) { m2v_simd_unreachable("setup_qw_mmx"); }
void setup_qw_sse2(void) { m2v_simd_unreachable("setup_qw_sse2"); }
void yuv422_to_bgr_mmx(void) { m2v_simd_unreachable("yuv422_to_bgr_mmx"); }
void yuv422_to_bgr_sse2(void) { m2v_simd_unreachable("yuv422_to_bgr_sse2"); }
void yuv422_to_yuy2_mmx(void) { m2v_simd_unreachable("yuv422_to_yuy2_mmx"); }
void yuv422_to_yuy2_sse2(void) { m2v_simd_unreachable("yuv422_to_yuy2_sse2"); }
void yuy2_convert_mmx(void) { m2v_simd_unreachable("yuy2_convert_mmx"); }
void yuy2_convert_sse2(void) { m2v_simd_unreachable("yuy2_convert_sse2"); }
