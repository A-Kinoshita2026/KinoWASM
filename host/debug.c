
#include "kinowasm.h"
#include "KinoUtil/kbuffer.h"
#include "kw_store.h"
#include "debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COMPARE_STRING(a, b) (strlen((const char*)a) == strlen((const char*)b) && memcmp(a, b, strlen(b)) == 0)

#if defined(KINOWASM_OPCODE_COUNTER)
extern uint64_t kinowasm_opcode_counter[0x10000];
extern uint64_t kinowasm_copy_next[0x200];
extern uint64_t kinowasm_copy_to_local_next[0x200];
#if defined(KINOWASM_OPCODE_PROFILE)
extern uint64_t kinowasm_opcode_cycles[0x10000];
#endif
#endif

/* opcode 名テーブルは KinoWASM/operand.h から tools/gen_opcode_names.py で
 * 自動生成する。enum を変更したら再生成すること。 */
static const char* debug_opcode_name(uint16_t opcode)
{
	switch (opcode) {
#include "debug_opcode_names.inc"
	default: return "OP_UNKNOWN";
	}
}

static int debug_opcode_has_ex(uint16_t opcode)
{
	if (opcode == OP_BLOCK || opcode == OP_LOOP || opcode == OP_IF)
		return 1;
	if (opcode == OP_BR || opcode == OP_BR_IF || opcode == OP_BR_TABLE)
		return 1;
	if (opcode == OP_CALL || opcode == OP_CALL_INDIRECT)
		return 1;
	if (opcode == OP_SELECT || opcode == OP_SELECT_T)
		return 1;
	if (opcode == OP_GLOBAL_GET || opcode == OP_GLOBAL_SET)
		return 1;
	if (opcode == OP_TABLE_GET || opcode == OP_TABLE_SET)
		return 1;
	if (opcode >= OP_I32_LOAD && opcode <= OP_I64_STORE32)
		return 1;
	if (opcode >= OP_I32_CONST && opcode <= OP_F64_CONST)
		return 1;
	if (opcode == OP_REF_FUNC)
		return 1;
	if (opcode >= OP_0XFC_I32_TRUNC_SAT_F32_S && opcode <= OP_0XFC_TABLE_FILL)
		return 1;
	return 0;
}

/* デコード済み命令 1 個の実サイズ。即値スロット (instr_ex_t) の有無だけで決まる
 * (ブロック構造スロットは廃止済み。パーサの ROLLBACK_MEMORY_COUNT と同一規約)。 */
static size_t debug_instr_size(uint16_t opcode)
{
	size_t size = CODE_SIZE;
	if (debug_opcode_has_ex(opcode))
		size += sizeof(instr_ex_t);

	return size;
}

static void debug_dump_instr_ex(uint16_t opcode, code_t ip)
{
	if (!debug_opcode_has_ex(opcode))
		return;

	code_ex_t instr_ex = (code_ex_t)(ip + 1);
	if (opcode == OP_I32_CONST) {
		printf(" imm=%d", instr_ex->c.i32);
	}
	else if (opcode == OP_CALL || opcode == OP_BR || opcode == OP_BR_IF) {
		printf(" ex=%u", instr_ex->val);
	}
	else if (opcode == OP_BLOCK || opcode == OP_LOOP || opcode == OP_IF) {
		printf(" blocktype=0x%08X", instr_ex->val);
	}
}

static kinowasm_result_t find_export(moduleinst_t* from, const char* name, externval_t* externval)
{
	_try{
		kinowasm_array_foreach(exportinst, exportinst_t, from->exports) {
			if (COMPARE_STRING(exportinst->name.data, name)) {
				*externval = exportinst->value;
				_throw(RES_SUCCESS);
			}
		}
		_throw(ERR_UNKNOWN_IMPORT);
	}
	_catch:
	return _result;
}

void kinowasm_dump_decoded_function(kinowasm_handle_t S, const char* module, const char* funcname)
{
	store_t* store = (store_t*)S;
	if (store == NULL || module == NULL || funcname == NULL) {
		printf("decoded dump invalid args\n");
		return;
	}

	kinowasm_array_foreach(mod, moduletable_t, store->moduletable) {
		if (strcmp(module, (const char*)mod->name.data) != 0)
			continue;

		externval_t externval;
		kinowasm_result_t res = find_export(mod->module, funcname, &externval);
		if (res != RES_SUCCESS || externval.kind != IMPORTDESC_FUNC) {
			printf("decoded dump target not found module=%s func=%s\n", module, funcname);
			return;
		}

		functioninstance_t* funcinst = &kinowasm_array_at(store->funcs, externval.func);
		code_t ip = funcinst->code->body;
		uint32_t depth = 0;
		uint32_t index = 0;

		printf("decoded dump module=%s func=%s funcaddr=%d\n", module, funcname, externval.func);
		for (;;) {
			uint16_t opcode = ip->opcode;
			int is_final_end = opcode == OP_END && depth == 0;
			printf(
				"decoded[%u] op=0x%04X %s r0=%u r1=%u r2=%u",
				index,
				opcode,
				debug_opcode_name(opcode),
				ip->r0,
				ip->r1,
				ip->r2
			);
			debug_dump_instr_ex(opcode, ip);
			printf("\n");

			if (opcode == OP_BLOCK || opcode == OP_LOOP || opcode == OP_IF)
				depth++;
			else if (opcode == OP_END && depth > 0)
				depth--;

			if (is_final_end)
				break;

			ip = (code_t)((uint8_t*)ip + debug_instr_size(opcode));
			index++;
			if (index > 512) {
				printf("decoded dump aborted: too many instructions\n");
				break;
			}
		}
		return;
	}

	printf("decoded dump module not found module=%s func=%s\n", module, funcname);
}

void kinowasm_reset_opcode_counter(void)
{
#if defined(KINOWASM_OPCODE_COUNTER)
	memset(kinowasm_opcode_counter, 0, sizeof(kinowasm_opcode_counter));
	memset(kinowasm_copy_next, 0, sizeof(kinowasm_copy_next));
	memset(kinowasm_copy_to_local_next, 0, sizeof(kinowasm_copy_to_local_next));
#if defined(KINOWASM_OPCODE_PROFILE)
	memset(kinowasm_opcode_cycles, 0, sizeof(kinowasm_opcode_cycles));
#endif
#endif
}

void kinowasm_dump_opcode_counter(void)
{
#if defined(KINOWASM_OPCODE_COUNTER)
	uint16_t top_opcode[16] = { 0 };
	uint64_t top_count[16] = { 0 };
	uint64_t total_count = 0;
	uint32_t unique_count = 0;

	for (uint32_t opcode = 0; opcode < 0x10000; opcode++) {
		uint64_t count = kinowasm_opcode_counter[opcode];
		if (count == 0)
			continue;

		total_count += count;
		unique_count++;

		for (size_t i = 0; i < sizeof(top_count) / sizeof(top_count[0]); i++) {
			if (count <= top_count[i])
				continue;

			for (size_t j = (sizeof(top_count) / sizeof(top_count[0])) - 1; j > i; j--) {
				top_count[j] = top_count[j - 1];
				top_opcode[j] = top_opcode[j - 1];
			}

			top_count[i] = count;
			top_opcode[i] = (uint16_t)opcode;
			break;
		}
	}

	printf(
		"opcode summary total=%llu unique=%u branch=%llu call=%llu memory=%llu\n",
		(unsigned long long)total_count,
		unique_count,
		(unsigned long long)(
			kinowasm_opcode_counter[OP_BLOCK] +
			kinowasm_opcode_counter[OP_LOOP] +
			kinowasm_opcode_counter[OP_IF] +
			kinowasm_opcode_counter[OP_ELSE] +
			kinowasm_opcode_counter[OP_END] +
			kinowasm_opcode_counter[OP_BR] +
			kinowasm_opcode_counter[OP_BR_IF] +
			kinowasm_opcode_counter[OP_BR_TABLE] +
			kinowasm_opcode_counter[OP_RETURN]
			),
		(unsigned long long)(
			kinowasm_opcode_counter[OP_CALL] +
			kinowasm_opcode_counter[OP_CALL_INDIRECT]
			),
		(unsigned long long)(
			kinowasm_opcode_counter[OP_I32_LOAD] +
			kinowasm_opcode_counter[OP_I64_LOAD] +
			kinowasm_opcode_counter[OP_F32_LOAD] +
			kinowasm_opcode_counter[OP_F64_LOAD] +
			kinowasm_opcode_counter[OP_I32_LOAD8_S] +
			kinowasm_opcode_counter[OP_I32_LOAD8_U] +
			kinowasm_opcode_counter[OP_I32_LOAD16_S] +
			kinowasm_opcode_counter[OP_I32_LOAD16_U] +
			kinowasm_opcode_counter[OP_I64_LOAD8_S] +
			kinowasm_opcode_counter[OP_I64_LOAD8_U] +
			kinowasm_opcode_counter[OP_I64_LOAD16_S] +
			kinowasm_opcode_counter[OP_I64_LOAD16_U] +
			kinowasm_opcode_counter[OP_I64_LOAD32_S] +
			kinowasm_opcode_counter[OP_I64_LOAD32_U] +
			kinowasm_opcode_counter[OP_I32_STORE] +
			kinowasm_opcode_counter[OP_I64_STORE] +
			kinowasm_opcode_counter[OP_F32_STORE] +
			kinowasm_opcode_counter[OP_F64_STORE] +
			kinowasm_opcode_counter[OP_I32_STORE8] +
			kinowasm_opcode_counter[OP_I32_STORE16] +
			kinowasm_opcode_counter[OP_I64_STORE8] +
			kinowasm_opcode_counter[OP_I64_STORE16] +
			kinowasm_opcode_counter[OP_I64_STORE32] +
			kinowasm_opcode_counter[OP_MEMORY_SIZE] +
			kinowasm_opcode_counter[OP_MEMORY_GROW] +
			kinowasm_opcode_counter[OP_0XFC_MEMORY_INIT] +
			kinowasm_opcode_counter[OP_0XFC_MEMORY_COPY] +
			kinowasm_opcode_counter[OP_0XFC_MEMORY_FILL]
			)
	);

	for (size_t i = 0; i < sizeof(top_count) / sizeof(top_count[0]); i++) {
		if (top_count[i] == 0)
			break;

		printf(
			"opcode[%zu] code=0x%04X count=%llu ratio=%.2f%%\n",
			i + 1,
			top_opcode[i],
			(unsigned long long)top_count[i],
			total_count == 0 ? 0.0 : ((double)top_count[i] * 100.0) / (double)total_count
		);
	}
	// COPY pair analysis
	{
		uint64_t copy_total = 0;
		uint64_t copy_to_local_total = 0;
		printf("\ncopy_next (what follows OP_COPY):\n");
		for (uint32_t i = 0; i < 0x200; i++) {
			if (kinowasm_copy_next[i] == 0) continue;
			copy_total += kinowasm_copy_next[i];
			copy_to_local_total += kinowasm_copy_to_local_next[i];
		}
		if (copy_total > 0) {
			for (uint32_t i = 0; i < 0x200; i++) {
				if (kinowasm_copy_next[i] == 0) continue;
				if ((double)kinowasm_copy_next[i] * 100.0 / (double)copy_total < 0.5) continue;
				uint64_t to_local = kinowasm_copy_to_local_next[i];
				uint64_t from_local = kinowasm_copy_next[i] - to_local;
				printf("  next=0x%04X count=%llu (to_local=%llu from_local=%llu) ratio=%.2f%%\n",
					i, (unsigned long long)kinowasm_copy_next[i],
					(unsigned long long)to_local, (unsigned long long)from_local,
					((double)kinowasm_copy_next[i] * 100.0) / (double)copy_total);
			}
		}
		printf("  copy_total=%llu (to_local=%llu from_local=%llu)\n",
			(unsigned long long)copy_total,
			(unsigned long long)copy_to_local_total,
			(unsigned long long)(copy_total - copy_to_local_total));
	}
#if defined(KINOWASM_OPCODE_PROFILE)
	// cycle profile: 累積実行サイクル上位 (どの opcode が重いか)
	{
		uint16_t top_op[16] = { 0 };
		uint64_t top_cyc[16] = { 0 };
		uint64_t total_cyc = 0;

		for (uint32_t opcode = 0; opcode < 0x10000; opcode++) {
			uint64_t cyc = kinowasm_opcode_cycles[opcode];
			if (cyc == 0)
				continue;

			total_cyc += cyc;

			for (size_t i = 0; i < sizeof(top_cyc) / sizeof(top_cyc[0]); i++) {
				if (cyc <= top_cyc[i])
					continue;

				for (size_t j = (sizeof(top_cyc) / sizeof(top_cyc[0])) - 1; j > i; j--) {
					top_cyc[j] = top_cyc[j - 1];
					top_op[j] = top_op[j - 1];
				}

				top_cyc[i] = cyc;
				top_op[i] = (uint16_t)opcode;
				break;
			}
		}

		printf("\ncycle profile total_cycles=%llu (includes rdtsc overhead; for relative comparison)\n",
			(unsigned long long)total_cyc);
		for (size_t i = 0; i < sizeof(top_cyc) / sizeof(top_cyc[0]); i++) {
			if (top_cyc[i] == 0)
				break;

			uint64_t cnt = kinowasm_opcode_counter[top_op[i]];
			printf(
				"cycle[%zu] code=0x%04X %s cycles=%llu ratio=%.2f%% avg=%.1f\n",
				i + 1,
				top_op[i],
				debug_opcode_name(top_op[i]),
				(unsigned long long)top_cyc[i],
				total_cyc == 0 ? 0.0 : ((double)top_cyc[i] * 100.0) / (double)total_cyc,
				cnt == 0 ? 0.0 : (double)top_cyc[i] / (double)cnt
			);
		}
	}
#endif
#else
	printf("opcode counter disabled\n");
#endif
}

/* count>0 の opcode をソートするための比較関数。profile 時は累積サイクル、
 * それ以外は実行回数で降順に並べる。 */
#if defined(KINOWASM_OPCODE_COUNTER)
static int debug_profile_cmp_desc(const void* a, const void* b)
{
	uint16_t oa = *(const uint16_t*)a;
	uint16_t ob = *(const uint16_t*)b;
#if defined(KINOWASM_OPCODE_PROFILE)
	uint64_t wa = kinowasm_opcode_cycles[oa];
	uint64_t wb = kinowasm_opcode_cycles[ob];
#else
	uint64_t wa = kinowasm_opcode_counter[oa];
	uint64_t wb = kinowasm_opcode_counter[ob];
#endif
	if (wa < wb) return 1;
	if (wa > wb) return -1;
	return 0;
}
#endif

void kinowasm_write_opcode_profile_csv(const char* path)
{
#if defined(KINOWASM_OPCODE_COUNTER)
	if (path == NULL) {
		printf("opcode profile CSV: path is NULL\n");
		return;
	}

	FILE* fp = fopen(path, "w");
	if (fp == NULL) {
		printf("opcode profile CSV open failed: %s\n", path);
		return;
	}

	uint64_t total_count = 0;
	uint64_t total_cycles = 0;
	for (uint32_t opcode = 0; opcode < 0x10000; opcode++) {
		total_count += kinowasm_opcode_counter[opcode];
#if defined(KINOWASM_OPCODE_PROFILE)
		total_cycles += kinowasm_opcode_cycles[opcode];
#endif
	}

	// count>0 の opcode を収集してソート (最大でも opcode 種別数なので少数)
	uint16_t* idx = (uint16_t*)malloc(0x10000 * sizeof(uint16_t));
	if (idx == NULL) {
		fclose(fp);
		printf("opcode profile CSV alloc failed\n");
		return;
	}

	uint32_t n = 0;
	for (uint32_t opcode = 0; opcode < 0x10000; opcode++) {
		if (kinowasm_opcode_counter[opcode] != 0)
			idx[n++] = (uint16_t)opcode;
	}
	qsort(idx, n, sizeof(idx[0]), debug_profile_cmp_desc);

	fprintf(fp, "opcode,name,count,cycles,cycles_pct,avg_cycles\n");
	for (uint32_t i = 0; i < n; i++) {
		uint16_t opcode = idx[i];
		uint64_t cnt = kinowasm_opcode_counter[opcode];
		uint64_t cyc = 0;
#if defined(KINOWASM_OPCODE_PROFILE)
		cyc = kinowasm_opcode_cycles[opcode];
#endif
		double pct = total_cycles == 0 ? 0.0 : ((double)cyc * 100.0) / (double)total_cycles;
		double avg = cnt == 0 ? 0.0 : (double)cyc / (double)cnt;
		fprintf(fp, "0x%04X,%s,%llu,%llu,%.4f,%.2f\n",
			opcode,
			debug_opcode_name(opcode),
			(unsigned long long)cnt,
			(unsigned long long)cyc,
			pct,
			avg);
	}

	free(idx);
	fclose(fp);
	printf("opcode profile CSV written: %s (opcodes=%u total_count=%llu total_cycles=%llu)\n",
		path, n,
		(unsigned long long)total_count,
		(unsigned long long)total_cycles);
#else
	(void)path;
	printf("opcode profile disabled (define KINOWASM_OPCODE_COUNTER or _PROFILE)\n");
#endif
}
