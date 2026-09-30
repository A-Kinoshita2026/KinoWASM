/* kw_parser_op_simd.inc — kw_parser_code_instr の命令デコード switch の一部 (0xFD prefix (SIMD, ~232 op))。
 * parser.c の BEGIN_PARSE ... END_PARSE の間に #include される。
 * 各 case は PARSE_OP(op) マクロ (= `case op:`) で始まる。単体では
 * コンパイルできない (parser.c の switch / マクロ文脈に依存)。 */
/* --- 0xFD Prefix (SIMD) --- */
PARSE_OP(OP_0XFD) {
#if !KINOWASM_ENABLE_SIMD
	/* SIMD が無効化されている: 0xFD prefix の opcode を含む wasm を reject。
	 * sub_op の読み取りは省略 (どうせ load を失敗させるため)。 */
	_throw(ERR_FEATURE_DISABLED);
#else
	uint32_t sub_op;
	_throwiferr(read_u32_leb(&sub_op, param->buf));
	/* #26: base + sub_op が uint16_t を超えると truncate して別 opcode に化けるため弾く。 */
	_throwif(RES_ERROR, sub_op > 0xFFFFu - OP_0XFD_V128_LOAD);
	param->instr->opcode = (uint16_t)(OP_0XFD_V128_LOAD + sub_op);
	switch(param->instr->opcode) {
	case OP_0XFD_V128_LOAD: {
		/* memarg を読み、i32 アドレスを pop して v128 を push する。
		 * natural alignment は 16 byte (= log2 4)。 */
		param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
		_throwif(ERR_MALFORMED_STORE, param->total_memories == 0);
		READ_MEMARG(instr_ex->memory);
		_throwif(ERR_MALFORMED_ALIGN, instr_ex->memory.align >= 32 || (1u << instr_ex->memory.align) > 16);
		uint16_t r1, r0;
		POP_REG(r1, TYPE_VAL_I32);
		PUSH_REG_V128(r0);
		param->instr->r0 = r0;
		param->instr->r1 = r1;
		break;
	}
	case OP_0XFD_V128_STORE: {
		/* memarg、v128 値、i32 アドレスを pop。 */
		param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
		_throwif(ERR_MALFORMED_STORE, param->total_memories == 0);
		READ_MEMARG(instr_ex->memory);
		_throwif(ERR_MALFORMED_ALIGN, instr_ex->memory.align >= 32 || (1u << instr_ex->memory.align) > 16);
		uint16_t r2, r1;
		POP_REG_V128(r2);
		POP_REG(r1, TYPE_VAL_I32);
		param->instr->r1 = r1;
		param->instr->r2 = r2;
		break;
	}
	/* v128.load extend (8x8/16x4/32x2 の s/u) / load_splat / load_zero:
	 * memarg を読み、i32 addr を pop、v128 を push。 */
	case OP_0XFD_V128_LOAD8X8_S:  case OP_0XFD_V128_LOAD8X8_U:
	case OP_0XFD_V128_LOAD16X4_S: case OP_0XFD_V128_LOAD16X4_U:
	case OP_0XFD_V128_LOAD32X2_S: case OP_0XFD_V128_LOAD32X2_U:
	case OP_0XFD_V128_LOAD8_SPLAT: case OP_0XFD_V128_LOAD16_SPLAT:
	case OP_0XFD_V128_LOAD32_SPLAT: case OP_0XFD_V128_LOAD64_SPLAT:
	case OP_0XFD_V128_LOAD32_ZERO: case OP_0XFD_V128_LOAD64_ZERO: {
		param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
		_throwif(ERR_MALFORMED_STORE, param->total_memories == 0);
		READ_MEMARG(instr_ex->memory);
		uint32_t access_size;
		switch(param->instr->opcode) {
		case OP_0XFD_V128_LOAD8_SPLAT: access_size = 1; break;
		case OP_0XFD_V128_LOAD16_SPLAT: access_size = 2; break;
		case OP_0XFD_V128_LOAD32_SPLAT:
		case OP_0XFD_V128_LOAD32_ZERO: access_size = 4; break;
		default: access_size = 8; break;  /* 8x8/16x4/32x2/64_splat/64_zero */
		}
		_throwif(ERR_MALFORMED_ALIGN, instr_ex->memory.align >= 32 || (1u << instr_ex->memory.align) > access_size);
		uint16_t r1, r0;
		POP_REG(r1, TYPE_VAL_I32);
		PUSH_REG_V128(r0);
		param->instr->r0 = r0;
		param->instr->r1 = r1;
		break;
	}
	/* v128.load{8,16,32,64}_lane: memarg + lane_idx (1 byte) を読み、
	 * i32 addr と v128 を pop、v128 を push。
	 * lane_idx は instr_ex の memarg と共用できないため kinowasm_mem_malloc で
	 * 確保した {memarg, lane_idx} 構造体ポインタを v128_ptr に格納。 */
	case OP_0XFD_V128_LOAD8_LANE: case OP_0XFD_V128_LOAD16_LANE:
	case OP_0XFD_V128_LOAD32_LANE: case OP_0XFD_V128_LOAD64_LANE:
	case OP_0XFD_V128_STORE8_LANE: case OP_0XFD_V128_STORE16_LANE:
	case OP_0XFD_V128_STORE32_LANE: case OP_0XFD_V128_STORE64_LANE: {
		param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
		_throwif(ERR_MALFORMED_STORE, param->total_memories == 0);
		simd_lane_imm_t* imm = kinowasm_mem_malloc(sizeof(simd_lane_imm_t));
		_throwif(ERR_OUTOFMEMORY, imm == NULL);
		READ_MEMARG(imm->memory);
		uint8_t lane_idx;
		_throwiferr(read_u8(&lane_idx, param->buf));
		uint32_t access_size;
		uint8_t lane_max;
		switch(param->instr->opcode) {
		case OP_0XFD_V128_LOAD8_LANE: case OP_0XFD_V128_STORE8_LANE:   access_size = 1; lane_max = 16; break;
		case OP_0XFD_V128_LOAD16_LANE: case OP_0XFD_V128_STORE16_LANE: access_size = 2; lane_max = 8; break;
		case OP_0XFD_V128_LOAD32_LANE: case OP_0XFD_V128_STORE32_LANE: access_size = 4; lane_max = 4; break;
		default:                                                       access_size = 8; lane_max = 2; break;
		}
		_throwif(ERR_MALFORMED_ALIGN, imm->memory.align >= 32 || (1u << imm->memory.align) > access_size);
		_throwif(ERR_MALFORMED_LANE, lane_idx >= lane_max);
		imm->lane_idx = lane_idx;
		instr_ex->v128_ptr = imm;
		uint8_t is_store = (param->instr->opcode >= OP_0XFD_V128_STORE8_LANE);
		uint16_t r2, r1, r0;
		if(is_store) {
			POP_REG_V128(r2);
			POP_REG(r1, TYPE_VAL_I32);
			param->instr->r1 = r1;
			param->instr->r2 = r2;
		} else {
			POP_REG_V128(r2);
			POP_REG(r1, TYPE_VAL_I32);
			PUSH_REG_V128(r0);
			param->instr->r0 = r0;
			param->instr->r1 = r1;
			param->instr->r2 = r2;
		}
		break;
	}
	case OP_0XFD_V128_CONST: {
		/* 16 byte 即値を kinowasm_mem_malloc で確保し、ポインタを instr_ex->v128_ptr
		 * に格納する。func / init expr どちらでも同じ op で扱える。 */
		param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
		kinowasm_v128_t* v = kinowasm_mem_malloc(sizeof(kinowasm_v128_t));
		_throwif(ERR_OUTOFMEMORY, v == NULL);
		_throwiferr(read_bytes(v->u8, 16, param->buf));
		instr_ex->v128_ptr = v;
		uint16_t r0;
		PUSH_REG_V128(r0);
		param->instr->r0 = r0;
		break;
	}
	/* iNxM extend low/high: v128 -> v128 (lane 幅倍化、半分のレーンを使う)。 */
	case OP_0XFD_I16X8_EXTEND_LOW_I8X16_S: case OP_0XFD_I16X8_EXTEND_HIGH_I8X16_S:
	case OP_0XFD_I16X8_EXTEND_LOW_I8X16_U: case OP_0XFD_I16X8_EXTEND_HIGH_I8X16_U:
	case OP_0XFD_I32X4_EXTEND_LOW_I16X8_S: case OP_0XFD_I32X4_EXTEND_HIGH_I16X8_S:
	case OP_0XFD_I32X4_EXTEND_LOW_I16X8_U: case OP_0XFD_I32X4_EXTEND_HIGH_I16X8_U:
	case OP_0XFD_I64X2_EXTEND_LOW_I32X4_S: case OP_0XFD_I64X2_EXTEND_HIGH_I32X4_S:
	case OP_0XFD_I64X2_EXTEND_LOW_I32X4_U: case OP_0XFD_I64X2_EXTEND_HIGH_I32X4_U:
	/* 浮動小数点 promote/demote / 丸め / 整数 ⇔ 浮動小数点 変換。 */
	case OP_0XFD_F32X4_DEMOTE_F64X2_ZERO: case OP_0XFD_F64X2_PROMOTE_LOW_F32X4:
	case OP_0XFD_F32X4_CEIL: case OP_0XFD_F32X4_FLOOR: case OP_0XFD_F32X4_TRUNC: case OP_0XFD_F32X4_NEAREST:
	case OP_0XFD_F64X2_CEIL: case OP_0XFD_F64X2_FLOOR: case OP_0XFD_F64X2_TRUNC: case OP_0XFD_F64X2_NEAREST:
	case OP_0XFD_I32X4_TRUNC_SAT_F32X4_S: case OP_0XFD_I32X4_TRUNC_SAT_F32X4_U:
	case OP_0XFD_F32X4_CONVERT_I32X4_S: case OP_0XFD_F32X4_CONVERT_I32X4_U:
	case OP_0XFD_I32X4_TRUNC_SAT_F64X2_S_ZERO: case OP_0XFD_I32X4_TRUNC_SAT_F64X2_U_ZERO:
	case OP_0XFD_F64X2_CONVERT_LOW_I32X4_S: case OP_0XFD_F64X2_CONVERT_LOW_I32X4_U:
	/* extadd_pairwise: v128 -> v128 (隣接レーン 2 つを加算して半数の倍幅 lane に)。 */
	case OP_0XFD_I16X8_EXTADD_PAIRWISE_I8X16_S: case OP_0XFD_I16X8_EXTADD_PAIRWISE_I8X16_U:
	case OP_0XFD_I32X4_EXTADD_PAIRWISE_I16X8_S: case OP_0XFD_I32X4_EXTADD_PAIRWISE_I16X8_U: {
		uint16_t r1, r0;
		POP_REG_V128(r1);
		PUSH_REG_V128(r0);
		param->instr->r0 = r0;
		param->instr->r1 = r1;
		break;
	}
	/* narrow (saturating): 2 v128 -> v128 (lane 幅半減、saturate)。 */
	case OP_0XFD_I8X16_NARROW_I16X8_S: case OP_0XFD_I8X16_NARROW_I16X8_U:
	case OP_0XFD_I16X8_NARROW_I32X4_S: case OP_0XFD_I16X8_NARROW_I32X4_U:
	/* q15mulr / extmul / dot: 2 v128 -> v128。 */
	case OP_0XFD_I16X8_Q15MULR_SAT_S:
	case OP_0XFD_I16X8_EXTMUL_LOW_I8X16_S: case OP_0XFD_I16X8_EXTMUL_HIGH_I8X16_S:
	case OP_0XFD_I16X8_EXTMUL_LOW_I8X16_U: case OP_0XFD_I16X8_EXTMUL_HIGH_I8X16_U:
	case OP_0XFD_I32X4_EXTMUL_LOW_I16X8_S: case OP_0XFD_I32X4_EXTMUL_HIGH_I16X8_S:
	case OP_0XFD_I32X4_EXTMUL_LOW_I16X8_U: case OP_0XFD_I32X4_EXTMUL_HIGH_I16X8_U:
	case OP_0XFD_I64X2_EXTMUL_LOW_I32X4_S: case OP_0XFD_I64X2_EXTMUL_HIGH_I32X4_S:
	case OP_0XFD_I64X2_EXTMUL_LOW_I32X4_U: case OP_0XFD_I64X2_EXTMUL_HIGH_I32X4_U:
	case OP_0XFD_I32X4_DOT_I16X8_S: {
		uint16_t r2, r1, r0;
		POP_REG_V128(r2);
		POP_REG_V128(r1);
		PUSH_REG_V128(r0);
		param->instr->r0 = r0;
		param->instr->r1 = r1;
		param->instr->r2 = r2;
		break;
	}
	/* iNxM 単項 (v128 -> v128): neg / popcnt / abs / fNxM 単項 (abs/neg/sqrt)。 */
	case OP_0XFD_I8X16_NEG:
	case OP_0XFD_I8X16_POPCNT:
	case OP_0XFD_I8X16_ABS:
	case OP_0XFD_I16X8_NEG:
	case OP_0XFD_I16X8_ABS:
	case OP_0XFD_I32X4_NEG:
	case OP_0XFD_I32X4_ABS:
	case OP_0XFD_I64X2_NEG:
	case OP_0XFD_I64X2_ABS:
	case OP_0XFD_F32X4_ABS:
	case OP_0XFD_F32X4_NEG:
	case OP_0XFD_F32X4_SQRT:
	case OP_0XFD_F64X2_ABS:
	case OP_0XFD_F64X2_NEG:
	case OP_0XFD_F64X2_SQRT: {
		uint16_t r1, r0;
		POP_REG_V128(r1);
		PUSH_REG_V128(r0);
		param->instr->r0 = r0;
		param->instr->r1 = r1;
		break;
	}
	/* iNxM 二項 (v128, v128 -> v128): add / sub / mul / min / max / avgr 系。 */
	case OP_0XFD_I8X16_ADD:
	case OP_0XFD_I8X16_SUB:
	case OP_0XFD_I8X16_MIN_S:
	case OP_0XFD_I8X16_MIN_U:
	case OP_0XFD_I8X16_MAX_S:
	case OP_0XFD_I8X16_MAX_U:
	case OP_0XFD_I8X16_AVGR_U:
	case OP_0XFD_I16X8_ADD:
	case OP_0XFD_I16X8_SUB:
	case OP_0XFD_I16X8_MUL:
	case OP_0XFD_I16X8_MIN_S:
	case OP_0XFD_I16X8_MIN_U:
	case OP_0XFD_I16X8_MAX_S:
	case OP_0XFD_I16X8_MAX_U:
	case OP_0XFD_I16X8_AVGR_U:
	case OP_0XFD_I32X4_ADD:
	case OP_0XFD_I32X4_SUB:
	case OP_0XFD_I32X4_MUL:
	case OP_0XFD_I32X4_MIN_S:
	case OP_0XFD_I32X4_MIN_U:
	case OP_0XFD_I32X4_MAX_S:
	case OP_0XFD_I32X4_MAX_U:
	case OP_0XFD_I64X2_ADD:
	case OP_0XFD_I64X2_SUB:
	case OP_0XFD_I64X2_MUL:
	case OP_0XFD_I8X16_ADD_SAT_S: case OP_0XFD_I8X16_ADD_SAT_U:
	case OP_0XFD_I8X16_SUB_SAT_S: case OP_0XFD_I8X16_SUB_SAT_U:
	case OP_0XFD_I16X8_ADD_SAT_S: case OP_0XFD_I16X8_ADD_SAT_U:
	case OP_0XFD_I16X8_SUB_SAT_S: case OP_0XFD_I16X8_SUB_SAT_U:
	case OP_0XFD_F32X4_ADD: case OP_0XFD_F32X4_SUB:
	case OP_0XFD_F32X4_MUL: case OP_0XFD_F32X4_DIV:
	case OP_0XFD_F32X4_MIN: case OP_0XFD_F32X4_MAX:
	case OP_0XFD_F32X4_PMIN: case OP_0XFD_F32X4_PMAX:
	case OP_0XFD_F64X2_ADD: case OP_0XFD_F64X2_SUB:
	case OP_0XFD_F64X2_MUL: case OP_0XFD_F64X2_DIV:
	case OP_0XFD_F64X2_MIN: case OP_0XFD_F64X2_MAX:
	case OP_0XFD_F64X2_PMIN: case OP_0XFD_F64X2_PMAX:
	case OP_0XFD_I8X16_SWIZZLE:
	/* 比較 (v128, v128 -> v128 マスク)。 */
	case OP_0XFD_I8X16_EQ: case OP_0XFD_I8X16_NE:
	case OP_0XFD_I8X16_LT_S: case OP_0XFD_I8X16_LT_U:
	case OP_0XFD_I8X16_GT_S: case OP_0XFD_I8X16_GT_U:
	case OP_0XFD_I8X16_LE_S: case OP_0XFD_I8X16_LE_U:
	case OP_0XFD_I8X16_GE_S: case OP_0XFD_I8X16_GE_U:
	case OP_0XFD_I16X8_EQ: case OP_0XFD_I16X8_NE:
	case OP_0XFD_I16X8_LT_S: case OP_0XFD_I16X8_LT_U:
	case OP_0XFD_I16X8_GT_S: case OP_0XFD_I16X8_GT_U:
	case OP_0XFD_I16X8_LE_S: case OP_0XFD_I16X8_LE_U:
	case OP_0XFD_I16X8_GE_S: case OP_0XFD_I16X8_GE_U:
	case OP_0XFD_I32X4_EQ: case OP_0XFD_I32X4_NE:
	case OP_0XFD_I32X4_LT_S: case OP_0XFD_I32X4_LT_U:
	case OP_0XFD_I32X4_GT_S: case OP_0XFD_I32X4_GT_U:
	case OP_0XFD_I32X4_LE_S: case OP_0XFD_I32X4_LE_U:
	case OP_0XFD_I32X4_GE_S: case OP_0XFD_I32X4_GE_U:
	case OP_0XFD_I64X2_EQ: case OP_0XFD_I64X2_NE:
	case OP_0XFD_I64X2_LT_S: case OP_0XFD_I64X2_GT_S:
	case OP_0XFD_I64X2_LE_S: case OP_0XFD_I64X2_GE_S:
	case OP_0XFD_F32X4_EQ: case OP_0XFD_F32X4_NE:
	case OP_0XFD_F32X4_LT: case OP_0XFD_F32X4_GT:
	case OP_0XFD_F32X4_LE: case OP_0XFD_F32X4_GE:
	case OP_0XFD_F64X2_EQ: case OP_0XFD_F64X2_NE:
	case OP_0XFD_F64X2_LT: case OP_0XFD_F64X2_GT:
	case OP_0XFD_F64X2_LE: case OP_0XFD_F64X2_GE: {
		uint16_t r2, r1, r0;
		POP_REG_V128(r2);
		POP_REG_V128(r1);
		PUSH_REG_V128(r0);
		param->instr->r0 = r0;
		param->instr->r1 = r1;
		param->instr->r2 = r2;
		break;
	}
	/* splat: scalar -> v128。 */
	case OP_0XFD_I8X16_SPLAT:
	case OP_0XFD_I16X8_SPLAT:
	case OP_0XFD_I32X4_SPLAT:
	case OP_0XFD_I64X2_SPLAT:
	case OP_0XFD_F32X4_SPLAT:
	case OP_0XFD_F64X2_SPLAT: {
		uint8_t scalar_type;
		switch(param->instr->opcode) {
		case OP_0XFD_I8X16_SPLAT:
		case OP_0XFD_I16X8_SPLAT:
		case OP_0XFD_I32X4_SPLAT: scalar_type = TYPE_VAL_I32; break;
		case OP_0XFD_I64X2_SPLAT: scalar_type = TYPE_VAL_I64; break;
		case OP_0XFD_F32X4_SPLAT: scalar_type = TYPE_VAL_F32; break;
		default:                  scalar_type = TYPE_VAL_F64; break;
		}
		uint16_t r1, r0;
		POP_REG(r1, scalar_type);
		PUSH_REG_V128(r0);
		param->instr->r0 = r0;
		param->instr->r1 = r1;
		break;
	}
	/* extract_lane: v128 -> scalar (lane idx は 1 byte 即値, instr_ex->x に格納)。 */
	case OP_0XFD_I8X16_EXTRACT_LANE_S:
	case OP_0XFD_I8X16_EXTRACT_LANE_U:
	case OP_0XFD_I16X8_EXTRACT_LANE_S:
	case OP_0XFD_I16X8_EXTRACT_LANE_U:
	case OP_0XFD_I32X4_EXTRACT_LANE:
	case OP_0XFD_I64X2_EXTRACT_LANE:
	case OP_0XFD_F32X4_EXTRACT_LANE:
	case OP_0XFD_F64X2_EXTRACT_LANE: {
		param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
		uint8_t lane_idx;
		_throwiferr(read_u8(&lane_idx, param->buf));
		uint8_t lane_max;
		uint8_t scalar_type;
		switch(param->instr->opcode) {
		case OP_0XFD_I8X16_EXTRACT_LANE_S:
		case OP_0XFD_I8X16_EXTRACT_LANE_U: lane_max = 16; scalar_type = TYPE_VAL_I32; break;
		case OP_0XFD_I16X8_EXTRACT_LANE_S:
		case OP_0XFD_I16X8_EXTRACT_LANE_U: lane_max = 8;  scalar_type = TYPE_VAL_I32; break;
		case OP_0XFD_I32X4_EXTRACT_LANE:   lane_max = 4;  scalar_type = TYPE_VAL_I32; break;
		case OP_0XFD_I64X2_EXTRACT_LANE:   lane_max = 2;  scalar_type = TYPE_VAL_I64; break;
		case OP_0XFD_F32X4_EXTRACT_LANE:   lane_max = 4;  scalar_type = TYPE_VAL_F32; break;
		default:                           lane_max = 2;  scalar_type = TYPE_VAL_F64; break;
		}
		_throwif(ERR_MALFORMED_LANE, lane_idx >= lane_max);
		instr_ex->x = lane_idx;
		uint16_t r1, r0;
		POP_REG_V128(r1);
		PUSH_REG(r0, scalar_type);
		param->instr->r0 = r0;
		param->instr->r1 = r1;
		break;
	}
	/* replace_lane: (v128, scalar) -> v128。 */
	case OP_0XFD_I8X16_REPLACE_LANE:
	case OP_0XFD_I16X8_REPLACE_LANE:
	case OP_0XFD_I32X4_REPLACE_LANE:
	case OP_0XFD_I64X2_REPLACE_LANE:
	case OP_0XFD_F32X4_REPLACE_LANE:
	case OP_0XFD_F64X2_REPLACE_LANE: {
		param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
		uint8_t lane_idx;
		_throwiferr(read_u8(&lane_idx, param->buf));
		uint8_t lane_max;
		uint8_t scalar_type;
		switch(param->instr->opcode) {
		case OP_0XFD_I8X16_REPLACE_LANE: lane_max = 16; scalar_type = TYPE_VAL_I32; break;
		case OP_0XFD_I16X8_REPLACE_LANE: lane_max = 8;  scalar_type = TYPE_VAL_I32; break;
		case OP_0XFD_I32X4_REPLACE_LANE: lane_max = 4;  scalar_type = TYPE_VAL_I32; break;
		case OP_0XFD_I64X2_REPLACE_LANE: lane_max = 2;  scalar_type = TYPE_VAL_I64; break;
		case OP_0XFD_F32X4_REPLACE_LANE: lane_max = 4;  scalar_type = TYPE_VAL_F32; break;
		default:                         lane_max = 2;  scalar_type = TYPE_VAL_F64; break;
		}
		_throwif(ERR_MALFORMED_LANE, lane_idx >= lane_max);
		instr_ex->x = lane_idx;
		uint16_t r2, r1, r0;
		POP_REG(r2, scalar_type);
		POP_REG_V128(r1);
		PUSH_REG_V128(r0);
		param->instr->r0 = r0;
		param->instr->r1 = r1;
		param->instr->r2 = r2;
		break;
	}
	/* i8x16.shuffle: 16 byte lane mask 即値 + 2 v128 引数 -> v128。
	 * mask は instr_ex->v128_ptr が指す 16 byte 領域に格納。各 byte は [0..32) 範囲。 */
	case OP_0XFD_I8X16_SHUFFLE: {
		param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
		kinowasm_v128_t* mask = kinowasm_mem_malloc(sizeof(kinowasm_v128_t));
		_throwif(ERR_OUTOFMEMORY, mask == NULL);
		_throwiferr(read_bytes(mask->u8, 16, param->buf));
		for(int i = 0; i < 16; i++)
			_throwif(ERR_MALFORMED_LANE, mask->u8[i] >= 32);
		instr_ex->v128_ptr = mask;
		uint16_t r2, r1, r0;
		POP_REG_V128(r2);
		POP_REG_V128(r1);
		PUSH_REG_V128(r0);
		param->instr->r0 = r0;
		param->instr->r1 = r1;
		param->instr->r2 = r2;
		break;
	}
	/* iNxM シフト: (v128, i32 count) -> v128。 */
	case OP_0XFD_I8X16_SHL: case OP_0XFD_I8X16_SHR_S: case OP_0XFD_I8X16_SHR_U:
	case OP_0XFD_I16X8_SHL: case OP_0XFD_I16X8_SHR_S: case OP_0XFD_I16X8_SHR_U:
	case OP_0XFD_I32X4_SHL: case OP_0XFD_I32X4_SHR_S: case OP_0XFD_I32X4_SHR_U:
	case OP_0XFD_I64X2_SHL: case OP_0XFD_I64X2_SHR_S: case OP_0XFD_I64X2_SHR_U: {
		uint16_t r2, r1, r0;
		POP_REG(r2, TYPE_VAL_I32);
		POP_REG_V128(r1);
		PUSH_REG_V128(r0);
		param->instr->r0 = r0;
		param->instr->r1 = r1;
		param->instr->r2 = r2;
		break;
	}
	/* v128.any_true / iNxM.all_true / iNxM.bitmask: v128 -> i32 (boolean / mask)。 */
	case OP_0XFD_V128_ANY_TRUE:
	case OP_0XFD_I8X16_ALL_TRUE:
	case OP_0XFD_I8X16_BITMASK:
	case OP_0XFD_I16X8_ALL_TRUE:
	case OP_0XFD_I16X8_BITMASK:
	case OP_0XFD_I32X4_ALL_TRUE:
	case OP_0XFD_I32X4_BITMASK:
	case OP_0XFD_I64X2_ALL_TRUE:
	case OP_0XFD_I64X2_BITMASK: {
		uint16_t r1, r0;
		POP_REG_V128(r1);
		PUSH_REG(r0, TYPE_VAL_I32);
		param->instr->r0 = r0;
		param->instr->r1 = r1;
		break;
	}
	/* v128.not: v128 -> v128 (単項)。 */
	case OP_0XFD_V128_NOT: {
		uint16_t r1, r0;
		POP_REG_V128(r1);
		PUSH_REG_V128(r0);
		param->instr->r0 = r0;
		param->instr->r1 = r1;
		break;
	}
	/* v128.and / andnot / or / xor: v128 二項。 */
	case OP_0XFD_V128_AND:
	case OP_0XFD_V128_ANDNOT:
	case OP_0XFD_V128_OR:
	case OP_0XFD_V128_XOR: {
		uint16_t r2, r1, r0;
		POP_REG_V128(r2);
		POP_REG_V128(r1);
		PUSH_REG_V128(r0);
		param->instr->r0 = r0;
		param->instr->r1 = r1;
		param->instr->r2 = r2;
		break;
	}
	/* v128.bitselect: v128 三項 (a, b, mask) -> a & mask | b & ~mask。
	 * register field 不足のため mask を instr_ex->x に格納。 */
	case OP_0XFD_V128_BITSELECT:
	/* WASM 3.0 Relaxed SIMD 三項 (a, b, mask) -> v128。bitselect と同じ shape。 */
	case OP_0XFD_I8X16_RELAXED_LANESELECT:
	case OP_0XFD_I16X8_RELAXED_LANESELECT:
	case OP_0XFD_I32X4_RELAXED_LANESELECT:
	case OP_0XFD_I64X2_RELAXED_LANESELECT:
	case OP_0XFD_F32X4_RELAXED_MADD:
	case OP_0XFD_F32X4_RELAXED_NMADD:
	case OP_0XFD_F64X2_RELAXED_MADD:
	case OP_0XFD_F64X2_RELAXED_NMADD:
	case OP_0XFD_I32X4_RELAXED_DOT_I8X16_I7X16_ADD_S: {
		param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
		uint16_t r3, r2, r1, r0;
		POP_REG_V128(r3);  /* mask / c */
		POP_REG_V128(r2);  /* b */
		POP_REG_V128(r1);  /* a */
		PUSH_REG_V128(r0);
		param->instr->r0 = r0;
		param->instr->r1 = r1;
		param->instr->r2 = r2;
		instr_ex->x = r3;
		break;
	}
	/* WASM 3.0 Relaxed SIMD 二項 (a, b) -> v128。strict 版と同じ shape。 */
	case OP_0XFD_I8X16_RELAXED_SWIZZLE:
	case OP_0XFD_F32X4_RELAXED_MIN:
	case OP_0XFD_F32X4_RELAXED_MAX:
	case OP_0XFD_F64X2_RELAXED_MIN:
	case OP_0XFD_F64X2_RELAXED_MAX:
	case OP_0XFD_I16X8_RELAXED_Q15MULR_S:
	case OP_0XFD_I16X8_RELAXED_DOT_I8X16_I7X16_S: {
		uint16_t r2, r1, r0;
		POP_REG_V128(r2);
		POP_REG_V128(r1);
		PUSH_REG_V128(r0);
		param->instr->r0 = r0;
		param->instr->r1 = r1;
		param->instr->r2 = r2;
		break;
	}
	/* WASM 3.0 Relaxed SIMD 単項 (v128) -> v128。 */
	case OP_0XFD_I32X4_RELAXED_TRUNC_F32X4_S:
	case OP_0XFD_I32X4_RELAXED_TRUNC_F32X4_U:
	case OP_0XFD_I32X4_RELAXED_TRUNC_F64X2_S_ZERO:
	case OP_0XFD_I32X4_RELAXED_TRUNC_F64X2_U_ZERO: {
		uint16_t r1, r0;
		POP_REG_V128(r1);
		PUSH_REG_V128(r0);
		param->instr->r0 = r0;
		param->instr->r1 = r1;
		break;
	}
	default:
		_throw(RES_ERROR);
	}
#endif /* KINOWASM_ENABLE_SIMD */
	break;
}
