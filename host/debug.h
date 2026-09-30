#pragma once

void kinowasm_dump_decoded_function(kinowasm_handle_t S, const char* module, const char* funcname);
void kinowasm_reset_opcode_counter(void);
void kinowasm_dump_opcode_counter(void);
/* opcode 別の実行回数 (+ KINOWASM_OPCODE_PROFILE 時は累積サイクル) を CSV に書き出す。
 * ヘッドレス実行でも結果ファイルとして回収できる。 */
void kinowasm_write_opcode_profile_csv(const char* path);
