#pragma once

#include <stdint.h>
#include <stddef.h>
#include "exception.h"

/* kinowasm_buf_t — 読み取りカーソル付きバイトバッファ。WASM バイナリの逐次読み取り
 * (u8 / バイト列 / f32・f64 / LEB128) に使う。kinowasm_buf_read_* が cur を前進させ、
 * 範囲外読み取りは ERR で弾く。kinowasm_buf_is_eof で終端判定。 */
typedef struct kbuffer {
	uint8_t* data;	/* バッファ先頭 (所有しない。呼び出し側が確保) */
	uint64_t cur;	/* 現在の読み取り位置 (data からのオフセット byte) */
	uint64_t len;	/* バッファ全長 (byte) */
} kinowasm_buf_t;

void kinowasm_buf_set(kinowasm_buf_t* buf, void* p, size_t size);
kinowasm_result_t kinowasm_buf_read_u8(uint8_t* dest, kinowasm_buf_t* buf);
kinowasm_result_t kinowasm_buf_read_bytes(void* dest, size_t size, kinowasm_buf_t* buf);
/* size バイト読み飛ばす (中身を使わない custom section など)。 */
kinowasm_result_t kinowasm_buf_skip(size_t size, kinowasm_buf_t* buf);
kinowasm_result_t kinowasm_buf_read_f32(float* dest, kinowasm_buf_t* buf);
kinowasm_result_t kinowasm_buf_read_f64(double* dest, kinowasm_buf_t* buf);
kinowasm_result_t kinowasm_buf_read_u_leb128(uint64_t* dest, uint32_t max_bits, kinowasm_buf_t* buf);
kinowasm_result_t kinowasm_buf_read_s_leb128(int64_t* dest, uint32_t max_bits, kinowasm_buf_t* buf);
kinowasm_result_t kinowasm_buf_read_u7_leb128(uint8_t* dest, kinowasm_buf_t* buf);
kinowasm_result_t kinowasm_buf_read_u32_leb128(uint32_t* dest, kinowasm_buf_t* buf);
kinowasm_result_t kinowasm_buf_read_s_64_leb128(int64_t* dest, kinowasm_buf_t* buf);
kinowasm_result_t kinowasm_buf_read_s_32_leb128(int32_t* dest, kinowasm_buf_t* buf);
int kinowasm_buf_is_eof(kinowasm_buf_t* buf);
