#pragma once

/* KinoWASM 内部 .c で繰り返し使う配列ユーティリティの短縮エイリアス。
 * kinowasm_array_* をそのまま書くと冗長なので、parser/kw_parser.c / kinowasm.c など
 * 内部ファイル用にここで一括定義する。
 *
 * 注: ヘッダ (.h) ではこれらの短い名前を export しない。
 *     "foreach" 等は名前衝突を起こしやすいため、内部 .c のみで使う。 */
#include "KinoUtil/karray.h"

#define array(type) kinowasm_array(type)
#define array_init(array) kinowasm_array_init_from(array)
#define array_term(array) kinowasm_array_term_from(array)
#define array_new(array, size) kinowasm_array_new_from(array, size)
#define array_at(array, index) kinowasm_array_at(array, index)
#define foreach(value, type, array) kinowasm_array_foreach(value, type, array)
#define foreach_reverse(value, type, array) kinowasm_array_foreach_reverse(value, type, array)
