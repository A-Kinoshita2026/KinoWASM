/* kw_parser_ops_ext.inc — kw_parser_code_instr の命令デコード switch の case 群 (プレフィックス拡張)。
 * 0xFC (trunc_sat/bulk memory/table/wide-arith) と 0xFE (atomics) を収録。
 * parser.c の BEGIN_PARSE ... END_PARSE の間に #include される。各 case は PARSE_OP(op)
 * マクロ (= `case op:`) で始まり、単体ではコンパイルできない (switch / マクロ文脈に依存)。 */
/* --- 0xFC Prefix --- */
PARSE_OP(OP_0XFC) {
	uint32_t op;
	_throwiferr(read_u32_leb(&op, param->buf));
	/* #26: op は attacker 制御 u32。base + op が uint16_t を超えると truncate して
	 * 別の有効 opcode に化けるため、cast 前に弾く (範囲内の未定義 sub-op は default:_throw)。 */
	_throwif(RES_ERROR, op > 0xFFFFu - OP_0XFC_I32_TRUNC_SAT_F32_S);
	param->instr->opcode = (uint16_t)(OP_0XFC_I32_TRUNC_SAT_F32_S + op);
	switch(param->instr->opcode) {
	case OP_0XFC_I32_TRUNC_SAT_F32_S: case OP_0XFC_I32_TRUNC_SAT_F32_U:
	case OP_0XFC_I32_TRUNC_SAT_F64_S: case OP_0XFC_I32_TRUNC_SAT_F64_U:
	case OP_0XFC_I64_TRUNC_SAT_F32_S: case OP_0XFC_I64_TRUNC_SAT_F32_U:
	case OP_0XFC_I64_TRUNC_SAT_F64_S: case OP_0XFC_I64_TRUNC_SAT_F64_U: {
		static const uint8_t sat_types[][2] = {
			{TYPE_VAL_F32,TYPE_VAL_I32}, {TYPE_VAL_F32,TYPE_VAL_I32},
			{TYPE_VAL_F64,TYPE_VAL_I32}, {TYPE_VAL_F64,TYPE_VAL_I32},
			{TYPE_VAL_F32,TYPE_VAL_I64}, {TYPE_VAL_F32,TYPE_VAL_I64},
			{TYPE_VAL_F64,TYPE_VAL_I64}, {TYPE_VAL_F64,TYPE_VAL_I64},
		};
		uint32_t idx = param->instr->opcode - OP_0XFC_I32_TRUNC_SAT_F32_S;
		PARSE_UNARY(sat_types[idx][0], sat_types[idx][1]);
		break;
	}
	case OP_0XFC_MEMORY_INIT: {
		param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
		/* WASM 3.0 multi-memory: dataidx + memidx を読む。 */
		uint32_t dataidx, memidx;
		_throwiferr(read_u32_leb(&dataidx, param->buf));
		_throwiferr(read_u32_leb(&memidx, param->buf));
		_throwif(ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS, param->total_memories == 0);
		_throwif(ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS, memidx >= param->total_memories);
		_throwif(ERR_DATASECTIONREQUIRED, param->total_datas <= dataidx);
		/* memory64: d は i64、s/n は i32。 */
		const memorytype_t* mt = get_memtype_by_idx(param->mod, memidx);
		uint8_t d_type = (mt && mt->is_64) ? TYPE_VAL_I64 : TYPE_VAL_I32;
		uint16_t r3, r2, r1;
		POP_REG(r3, TYPE_VAL_I32);  /* n */
		POP_REG(r2, TYPE_VAL_I32);  /* s */
		POP_REG(r1, d_type);        /* d */
		if(d_type == TYPE_VAL_I64) {
			/* memory64 用 OP_MEM64_BULK に rewrite。
			 * align=orig_op、memidx=memidx、offset=dataidx に格納。 */
			param->instr->opcode = OP_MEM64_BULK;
			param->instr->r0 = r3;
			param->instr->r1 = r1;
			param->instr->r2 = r2;
			instr_ex->memory.align = (uint16_t)OP_0XFC_MEMORY_INIT;
			instr_ex->memory.memidx = (uint16_t)memidx;
			instr_ex->memory.offset = dataidx;
		}
		else {
			param->instr->r0 = (uint16_t)memidx;  /* memidx を r0 に格納 */
			param->instr->r1 = r1;
			param->instr->r2 = r2;
			instr_ex->x = dataidx;
			instr_ex->y = r3;
		}
		break;
	}
	case OP_0XFC_DATA_DROP: {
		param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
		_throwiferr(read_u32_leb(&instr_ex->x, param->buf));
		_throwif(ERR_DATASECTIONREQUIRED, param->total_datas <= instr_ex->x);
		break;
	}
	case OP_0XFC_MEMORY_COPY:
	case OP_0XFC_MEMORY_FILL: {
		/* WASM 3.0 multi-memory:
		 * memory.copy: dst memidx + src memidx を読む。
		 * memory.fill: memidx 1 個を読む。
		 * 引数 r3 を r0 に、memidx を instr_ex->x (dst) / instr_ex->y (src) に保存
		 * (instr_ex->val と x/y は union 共有なので分離して使う)。 */
		uint32_t dst_memidx, src_memidx = 0;
		int is_copy = (param->instr->opcode == OP_0XFC_MEMORY_COPY);
		if(is_copy) {
			_throwiferr(read_u32_leb(&dst_memidx, param->buf));
			_throwiferr(read_u32_leb(&src_memidx, param->buf));
		}
		else {
			_throwiferr(read_u32_leb(&dst_memidx, param->buf));
		}
		_throwif(ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS, param->total_memories == 0);
		_throwif(ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS, dst_memidx >= param->total_memories);
		_throwif(ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS, src_memidx >= param->total_memories);
		/* memory64 検出。
		 * COPY: d=dst type、s=src type、n=min(dst,src) type。
		 * FILL: d=mem type、val=i32 固定、n=mem type。 */
		const memorytype_t* dst_mt = get_memtype_by_idx(param->mod, dst_memidx);
		const memorytype_t* src_mt = is_copy ? get_memtype_by_idx(param->mod, src_memidx) : dst_mt;
		uint8_t dst_64 = (dst_mt && dst_mt->is_64) ? 1 : 0;
		uint8_t src_64 = (src_mt && src_mt->is_64) ? 1 : 0;
		uint8_t d_type = dst_64 ? TYPE_VAL_I64 : TYPE_VAL_I32;
		uint8_t s_type;
		uint8_t n_type;
		if(is_copy) {
			s_type = src_64 ? TYPE_VAL_I64 : TYPE_VAL_I32;
			n_type = (dst_64 && src_64) ? TYPE_VAL_I64 : TYPE_VAL_I32;
		}
		else {
			s_type = TYPE_VAL_I32;  /* fill val */
			n_type = d_type;
		}
		uint16_t r3, r2, r1;
		POP_REG(r3, n_type);
		POP_REG(r2, s_type);
		POP_REG(r1, d_type);
		if(dst_64 || src_64) {
			/* memory64 用 OP_MEM64_BULK に rewrite。 */
			param->instr->opcode = OP_MEM64_BULK;
			param->instr->r0 = r3;
			param->instr->r1 = r1;
			param->instr->r2 = r2;
			param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
			instr_ex->memory.align = (uint16_t)(is_copy ? OP_0XFC_MEMORY_COPY : OP_0XFC_MEMORY_FILL);
			instr_ex->memory.memidx = (uint16_t)dst_memidx;
			instr_ex->memory.offset = src_memidx;  /* COPY のみ意味あり */
		}
		else {
			param->instr->r0 = r3;
			param->instr->r1 = r1;
			param->instr->r2 = r2;
			param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
			instr_ex->x = dst_memidx;
			instr_ex->y = src_memidx;
		}
		break;
	}
	case OP_0XFC_TABLE_INIT: {
		param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
		_throwiferr(read_u32_leb(&instr_ex->y, param->buf)); /* elem */
		_throwiferr(read_u32_leb(&instr_ex->x, param->buf)); /* table */
		_throwif(ERR_MALFORMED_TABLE, param->total_tables <= instr_ex->x);
		_throwif(ERR_MALFORMED_TABLE, param->total_elems <= instr_ex->y);
		uint8_t dst_idx_type;
		{
			tabletype_t* t1 = &array_at((*param->tables), instr_ex->x);
			element_t* t2 = &array_at(param->mod->elements, instr_ex->y);
			_throwif(ERR_TYPE_MISMATCH, t1->reftype != t2->type);
			dst_idx_type = t1->limits.is_64 ? TYPE_VAL_I64 : TYPE_VAL_I32;
		}
		/* table.init: stack [dst_idx, src_idx, n] (top is n)
		 *   dst_idx は table の index 型 (i32 or i64)、src_idx と n は常に i32。 */
		uint16_t r3, r2, r1;
		POP_REG(r3, TYPE_VAL_I32);  /* n */
		POP_REG(r2, TYPE_VAL_I32);  /* src */
		POP_REG(r1, dst_idx_type);  /* dst */
		param->instr->r1 = r1;
		param->instr->r2 = r2;
		param->instr->r0 = r3;
		break;
	}
	case OP_0XFC_ELEM_DROP: {
		param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
		_throwiferr(read_u32_leb(&instr_ex->x, param->buf));
		_throwif(ERR_TRAP_UNDEFINED_ELEMENT, param->total_elems <= instr_ex->x);
		break;
	}
	case OP_0XFC_TABLE_COPY: {
		param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
		_throwiferr(read_u32_leb(&instr_ex->x, param->buf)); /* dst */
		_throwiferr(read_u32_leb(&instr_ex->y, param->buf)); /* src */
		_throwif(ERR_MALFORMED_TABLE, param->total_tables <= instr_ex->x);
		_throwif(ERR_MALFORMED_TABLE, param->total_tables <= instr_ex->y);
		uint8_t dst_idx_type, src_idx_type, n_type;
		{
			tabletype_t* t1 = &array_at((*param->tables), instr_ex->x);
			tabletype_t* t2 = &array_at((*param->tables), instr_ex->y);
			_throwif(ERR_TYPE_MISMATCH, t1->reftype != t2->reftype);
			dst_idx_type = t1->limits.is_64 ? TYPE_VAL_I64 : TYPE_VAL_I32;
			src_idx_type = t2->limits.is_64 ? TYPE_VAL_I64 : TYPE_VAL_I32;
			/* n is i64 if both tables are 64-bit, otherwise i32 */
			n_type = (t1->limits.is_64 && t2->limits.is_64) ? TYPE_VAL_I64 : TYPE_VAL_I32;
		}
		/* table.copy: stack [dst, src, n] (top is n) */
		uint16_t r3, r2, r1;
		POP_REG(r3, n_type);
		POP_REG(r2, src_idx_type);
		POP_REG(r1, dst_idx_type);
		param->instr->r1 = r1;
		param->instr->r2 = r2;
		param->instr->r0 = r3;
		break;
	}
	case OP_0XFC_TABLE_GROW: {
		param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
		_throwiferr(read_u32_leb(&instr_ex->x, param->buf));
		_throwif(ERR_MALFORMED_TABLE, param->total_tables <= instr_ex->x);
		/* table.grow: stack [ref, n] -> [old_size]
		 *   table64 では n と old_size はともに i64。 */
		uint16_t r2, r1, r0;
		uint8_t sz_type;
		{
			tabletype_t* t1 = &array_at((*param->tables), instr_ex->x);
			sz_type = t1->limits.is_64 ? TYPE_VAL_I64 : TYPE_VAL_I32;
			POP_REG(r2, sz_type);
			POP_REG(r1, t1->reftype);
		}
		PUSH_REG(r0, sz_type);
		param->instr->r0 = r0;
		param->instr->r1 = r1;
		param->instr->r2 = r2;
		break;
	}
	case OP_0XFC_TABLE_SIZE: {
		param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
		_throwiferr(read_u32_leb(&instr_ex->x, param->buf));
		_throwif(ERR_MALFORMED_TABLE, param->total_tables <= instr_ex->x);
		uint8_t sz_type;
		{
			tabletype_t* t1 = &array_at((*param->tables), instr_ex->x);
			sz_type = t1->limits.is_64 ? TYPE_VAL_I64 : TYPE_VAL_I32;
		}
		uint16_t r0;
		PUSH_REG(r0, sz_type);
		param->instr->r0 = r0;
		break;
	}
	case OP_0XFC_TABLE_FILL: {
		param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
		_throwiferr(read_u32_leb(&instr_ex->x, param->buf));
		_throwif(ERR_MALFORMED_TABLE, param->total_tables <= instr_ex->x);
		/* table.fill: stack [i, ref, n] (top is n)
		 *   table64 では i と n はともに i64。 */
		uint16_t r3, r2, r1;
		uint8_t idx_type;
		{
			tabletype_t* t1 = &array_at((*param->tables), instr_ex->x);
			idx_type = t1->limits.is_64 ? TYPE_VAL_I64 : TYPE_VAL_I32;
			POP_REG(r3, idx_type);
			POP_REG(r2, t1->reftype);
			POP_REG(r1, idx_type);
		}
		param->instr->r1 = r1;
		param->instr->r2 = r2;
		param->instr->r0 = r3;
		break;
	}
	/* Wide arithmetic proposal:
	 * i64.add128 / i64.sub128 は (lo_a, hi_a, lo_b, hi_b) → (lo, hi)。
	 * i64.mul_wide_s / i64.mul_wide_u は (a, b) → (lo, hi)。
	 * いずれも入力と出力が同じレジスタ範囲を共有するため、ip->r0 に
	 * base register (= 入力先頭 = 出力先頭) を格納し、runtime 側で先に
	 * 全入力を読み出してから出力を書く。 */
	case OP_0XFC_I64_ADD128:
	case OP_0XFC_I64_SUB128: {
		UNUSE uint16_t r3, r2, r1, r0;
		POP_REG(r3, TYPE_VAL_I64);  /* hi_b (top) */
		POP_REG(r2, TYPE_VAL_I64);  /* lo_b */
		POP_REG(r1, TYPE_VAL_I64);  /* hi_a */
		POP_REG(r0, TYPE_VAL_I64);  /* lo_a */
		PUSH_REG(r0, TYPE_VAL_I64); /* lo_result */
		PUSH_REG(r1, TYPE_VAL_I64); /* hi_result */
		param->instr->r0 = r0;
		(void)r2; (void)r3;
		break;
	}
	case OP_0XFC_I64_MUL_WIDE_S:
	case OP_0XFC_I64_MUL_WIDE_U: {
		UNUSE uint16_t r1, r0;
		POP_REG(r1, TYPE_VAL_I64);  /* b (top) */
		POP_REG(r0, TYPE_VAL_I64);  /* a */
		PUSH_REG(r0, TYPE_VAL_I64); /* lo_result */
		PUSH_REG(r1, TYPE_VAL_I64); /* hi_result */
		param->instr->r0 = r0;
		break;
	}
	default:
		_throw(RES_ERROR);
	}
	break;
}
/* kw_parser_op_threads.inc — kw_parser_code_instr の命令デコード switch の一部 (0xFE prefix (WASM 3.0 threads & atomics))。
 * parser.c の BEGIN_PARSE ... END_PARSE の間に #include される。
 * 各 case は PARSE_OP(op) マクロ (= `case op:`) で始まる。単体では
 * コンパイルできない (parser.c の switch / マクロ文脈に依存)。 */
/* --- 0xFE Prefix (WASM 3.0 threads/atomics) --- */
PARSE_OP(OP_0XFE) {
	/* WASM threads & atomics (0xFE prefix)。core エンジンに 0xFE の実装が無く実行できない
	 * ため、ここで一元的に拒否する (デコードを通しても core build で必ず落ちる)。
	 * shared memory 宣言自体は atomics を使わなければ単一スレッドとして有効なので弾かない。 */
	_throw(ERR_FEATURE_DISABLED);
}
