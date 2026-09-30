
#include "kbuffer.h"

#include <string.h>

void kinowasm_buf_set(kinowasm_buf_t* buf, void* p, size_t size)
{
	buf->cur = 0;
	buf->data = (uint8_t*)p;
	buf->len = size;
}

kinowasm_result_t kinowasm_buf_read_u8(uint8_t* dest, kinowasm_buf_t* buf)
{
	_try{
		_throwif(ERR_UNEXPECTED_END, buf->cur + 1 > buf->len);
		*dest = *(buf->data + buf->cur);
		buf->cur++;
	}
	_catch:
	return _result;
}

kinowasm_result_t kinowasm_buf_read_bytes(void* dest, size_t size, kinowasm_buf_t* buf)
{
	_try{
		/* ★範囲を 1 回見てまとめて写す。1 バイトずつ
		 *   kinowasm_buf_read_u8 を回すと、毎回境界検査と例外判定が入り
		 *   極端に遅い。data section 3.8MB の読み取りに 4.4 秒かかっていた
		 *   (生成コードを積んだ wasm で起動が 12 秒になった主因の一つ)。 */
		_throwif(ERR_UNEXPECTED_END, buf->cur + (uint64_t)size > buf->len);
		if(size != 0)
			memcpy(dest, buf->data + buf->cur, size);
		buf->cur += size;
	}
	_catch:
	return _result;
}

/* size バイト読み飛ばす (中身を使わない custom section など)。 */
kinowasm_result_t kinowasm_buf_skip(size_t size, kinowasm_buf_t* buf)
{
	_try{
		_throwif(ERR_UNEXPECTED_END, buf->cur + (uint64_t)size > buf->len);
		buf->cur += size;
	}
	_catch:
	return _result;
}

kinowasm_result_t kinowasm_buf_read_f32(float* dest, kinowasm_buf_t* buf)
{
	_try{
		_throwif(ERR_UNEXPECTED_END, buf->cur + 4 > buf->len);
		*dest = *(float*)(buf->data + buf->cur);
		buf->cur += 4;
	}
	_catch:
	return _result;
}

kinowasm_result_t kinowasm_buf_read_f64(double* dest, kinowasm_buf_t* buf)
{
	_try{
		_throwif(ERR_UNEXPECTED_END, buf->cur + 8 > buf->len);
		*dest = *(double*)(buf->data + buf->cur);
		buf->cur += 8;
	}
	_catch:
	return _result;
}

/* Little Endian Base 128の符号なし整数版 */
kinowasm_result_t kinowasm_buf_read_u_leb128(uint64_t* dest, uint32_t max_bits, kinowasm_buf_t* buf)
{
	_try{
		uint64_t value = 0;
		uint32_t shift = 0;
		uint8_t b = 0;
		for(;;) {
			_throwiferr(kinowasm_buf_read_u8(&b, buf));
			value |= ((uint64_t)b & 0x7f) << shift;
			shift += 7;
			if((b & 0x80) == 0)
				break;

			if(shift >= max_bits)
				_throw(ERR_INTEGER_CONVERT_TOO_LONG);
		}
		/* 最終 byte の予約 bit 検査 (WASM spec compliance):
		 * 例えば u32 (max_bits=32) の 5 byte 目は 4 bit のみ有効、上位 3 bit が 0
		 * でなければ ERR_INTEGER_TOO_LARGE で reject。整数範囲チェックは呼出元
		 * (read_u32_leb128 等) でも行うが、ここでより早期に正確に弾く。 */
		if(shift > max_bits) {
			uint32_t consumed_bits = shift - 7;
			uint32_t valid_bits = max_bits - consumed_bits;
			uint8_t low_mask = (uint8_t)((1u << valid_bits) - 1);
			if((b & 0x7Fu & (uint8_t)~low_mask) != 0)
				_throw(ERR_INTEGER_TOO_LARGE);
		}
		*dest = value;
	}
	_catch:
	return _result;
}

/* Little Endian Base 128の符号付き整数版 */
kinowasm_result_t kinowasm_buf_read_s_leb128(int64_t* dest, uint32_t max_bits, kinowasm_buf_t* buf)
{
	_try{
		int64_t value = 0;
		uint32_t shift = 0;
		uint8_t b;
		for(;;) {
			_throwiferr(kinowasm_buf_read_u8(&b, buf));
			value |= ((uint64_t)b & 0x7f) << shift;
			shift += 7;
			if((b & 0x80) == 0)
				break;

			if(shift >= max_bits)
				_throw(ERR_INTEGER_CONVERT_TOO_LONG);
		}
		/* 最終 byte の予約 bit 検査 (WASM spec compliance、s32/s64 両対応):
		 * s_LEB の最終 byte の有効 bit は (max_bits - (k-1)*7) bit。それを超える
		 * 上位 bit は sign extension で全 0 (positive) または全 1 (negative)
		 * でなければならない。例: s32 5 byte 目は 4 bit 有効、上位 3 bit は
		 * sign bit と同じ値が必須 → b ∈ [0..0x07] (positive) または
		 * [0x78..0x7F] (negative)。 */
		if(shift > max_bits) {
			uint32_t consumed_bits = shift - 7;
			uint32_t valid_bits = max_bits - consumed_bits;
			uint8_t low_mask = (uint8_t)((1u << valid_bits) - 1);
			uint8_t sign_bit = (uint8_t)((b >> (valid_bits - 1)) & 1u);
			uint8_t expected_high = sign_bit ? (uint8_t)(0x7Fu & ~low_mask) : 0u;
			if((b & 0x7Fu & (uint8_t)~low_mask) != expected_high)
				_throw(ERR_INTEGER_TOO_LARGE);
		}

		/* 符号拡張マスクは 64bit 幅で作る。value は int64_t なので size_t が 32bit の
		 * 環境で (size_t)(-1) を使うとマスクが 32bit 幅に留まり、s64 負値の上位 bit が
		 * 立たない/幅超シフトが UB になる。uint64_t で常に 64bit 幅を確保する。 */
		if((shift < 64) && (b & 0x40) != 0)
			value |= ((uint64_t)(-1) << shift);

		*dest = value;
	}
	_catch:
	return _result;
}

kinowasm_result_t kinowasm_buf_read_u7_leb128(uint8_t* dest, kinowasm_buf_t* buf)
{
	_try{
		uint64_t num;
		_throwiferr(kinowasm_buf_read_u_leb128(&num, 7, buf));
		*dest = (uint8_t)num;
	}
	_catch:
	return _result;
}

kinowasm_result_t kinowasm_buf_read_u32_leb128(uint32_t* dest, kinowasm_buf_t* buf)
{
	_try{
		uint64_t num;
		_throwiferr(kinowasm_buf_read_u_leb128(&num, 32, buf));
		_throwif(ERR_INTEGER_TOO_LARGE, num > UINT32_MAX);
		*dest = (uint32_t)num;
	}
	_catch:
	return _result;
}

kinowasm_result_t kinowasm_buf_read_s_64_leb128(int64_t* dest, kinowasm_buf_t* buf)
{
	return kinowasm_buf_read_s_leb128(dest, 64, buf);
}

kinowasm_result_t kinowasm_buf_read_s_32_leb128(int32_t* dest, kinowasm_buf_t* buf)
{
	_try{
		int64_t num;
		_throwiferr(kinowasm_buf_read_s_leb128(&num, 32, buf));
		_throwif(ERR_INTEGER_TOO_LARGE, num > INT32_MAX || num < INT32_MIN);
		*dest = (int32_t)num;
	}
	_catch:
	return _result;
}

int kinowasm_buf_is_eof(kinowasm_buf_t* buf)
{
	if(buf->cur < buf->len)
		return 0;
	else
		return 1;
}
