/* kw_parser_ops_base.inc — kw_parser_code_instr の命令デコード switch の case 群 (基本命令)。
 * 制御 / 変数・テーブル・メモリ・定数 / 数値 命令を収録。
 * parser.c の BEGIN_PARSE ... END_PARSE の間に #include される。各 case は PARSE_OP(op)
 * マクロ (= `case op:`) で始まり、単体ではコンパイルできない (switch / マクロ文脈に依存)。 */
/* --- 制御命令 --- */
PARSE_OP(OP_UNREACHABLE)
	/* 現在フレーム上の具体型を捨て、以後は stack polymorphism として扱う */
	DROP_STACK_TO_POLYMORPHIC();
break;
PARSE_OP(OP_NOP)
	break;
PARSE_OP(OP_BLOCK)
PARSE_OP(OP_LOOP)
PARSE_OP(OP_IF)
PARSE_OP(OP_TRY)  /* WASM 3.0 exception handling: BLOCK 構造 + catch chain 保持 */
	param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
_throwiferr(read_blocktype(&instr_ex->blocktype, param->buf));

if(param->instr->opcode == OP_IF) {
	uint16_t r1;
	POP_REG(r1, TYPE_VAL_I32);
	param->instr->r1 = r1;
}

/* ブロック入力パラメータの型チェック */
{
	functiontype_t blk_ft;
	uint8_t blk_vt_buf[2];
	_throwiferr(kw_parser_resolve_blocktype(&blk_ft, instr_ex->blocktype, param->mod, blk_vt_buf));
	POP_RESULTTYPE_REVERSE(blk_ft.rt1);
	param->instr->r0 = (uint16_t)(param->total_locals + type_stack->idx + 1);
}
break;
PARSE_OP(OP_ELSE)
PARSE_OP(OP_END)
	break;
PARSE_OP(OP_BR) {
	uint32_t labelidx;
	param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
	_throwiferr(read_u32_leb(&labelidx, param->buf));
	_throwif(ERR_MALFORMED_BLOCK, labelidx >= param->label_depth);
	instr_ex->val = labelidx;
	{
		resulttype_t label_rt;
		uint8_t label_vt_buf[2];
		_throwiferr(kw_parser_get_label_resulttype(&label_rt, param, labelidx, label_vt_buf));
		param->instr->r0 = label_rt.len == 0 ? 0 : (uint16_t)(param->total_locals + type_stack->idx - (int32_t)label_rt.len + 1);
		POP_RESULTTYPE_REVERSE(label_rt);
	}
	DROP_STACK_TO_POLYMORPHIC();

	break;
}
PARSE_OP(OP_BR_IF) {
	uint32_t labelidx;
	param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
	_throwiferr(read_u32_leb(&labelidx, param->buf));
	_throwif(ERR_MALFORMED_BLOCK, labelidx >= param->label_depth);
	instr_ex->val = labelidx;
	uint16_t r1;
	POP_REG(r1, TYPE_VAL_I32);
	param->instr->r1 = r1;
	{
		resulttype_t label_rt;
		uint8_t label_vt_buf[2];
		_throwiferr(kw_parser_get_label_resulttype(&label_rt, param, labelidx, label_vt_buf));
		param->instr->r0 = label_rt.len == 0 ? 0 : (uint16_t)(param->total_locals + type_stack->idx - (int32_t)label_rt.len + 1);
		POP_RESULTTYPE_REVERSE(label_rt);
		PUSH_RESULTTYPE(label_rt);
	}
	break;
}
PARSE_OP(OP_BR_TABLE) {
	param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
	uint32_t label_size;
	_throwiferr(read_u32_leb(&label_size, param->buf));
	/* #23: 各 label は最低 1 byte の LEB なので、残バッファ size を超える label_size は
	 * 不正。array_new 前に弾いて巨大確保 (label_size*4) を防ぐ (TRY_TABLE と同じ guard)。 */
	_throwif(ERR_MALFORMED_FUNC, (uint64_t)label_size > param->buf->len - param->buf->cur);
	instr_ex->labels = kinowasm_mem_malloc(sizeof(u32karray_t));
	/* セキュリティ: malloc 失敗を見ないと array_new が NULL ポインタ経由で書き込む */
	_throwif(ERR_OUTOFMEMORY, instr_ex->labels == NULL);
	/* br_table の labels (struct + 本体) は decode/検証専用で core は raw bytes から再コンパイルする
	 * ため load 後は不要。本体スクラッチとは別の単発確保だが、module アリーナの wholesale 解放で
	 * 回収される。 */
	_throwiferr(array_new(*instr_ex->labels, label_size));
	foreach(label, uint32_t, *instr_ex->labels) {
		_throwiferr(read_u32_leb(label, param->buf));
		if(*label >= param->label_depth)
			_throw(ERR_MALFORMED_BLOCK);
	}
	uint32_t default_label;
	_throwiferr(read_u32_leb(&default_label, param->buf));
	if(default_label >= param->label_depth)
		_throw(ERR_MALFORMED_BLOCK);
	{
		resulttype_t default_rt;
		uint8_t default_vt_buf[2];
		_throwiferr(kw_parser_get_label_resulttype(&default_rt, param, default_label, default_vt_buf));
		uint16_t r1;
		POP_REG(r1, TYPE_VAL_I32);
		/* Unknown values may match different label types, but explicit values
		 * in a polymorphic stack must satisfy every target, not just default. */
		int32_t saved_idx = type_stack->idx;
		foreach(label, uint32_t, *instr_ex->labels) {
			resulttype_t label_rt;
			uint8_t label_vt_buf[2];
			_throwiferr(kw_parser_get_label_resulttype(&label_rt, param, *label, label_vt_buf));
			_throwif(ERR_TYPE_MISMATCH, label_rt.len != default_rt.len);
			POP_RESULTTYPE_REVERSE(label_rt);
			type_stack->idx = saved_idx;
		}
		param->instr->r0 = default_rt.len == 0 ? 0 : (uint16_t)(param->total_locals + type_stack->idx - (int32_t)default_rt.len + 1);
		param->instr->r1 = r1;
		param->instr->r2 = (uint16_t)default_label;

		POP_RESULTTYPE_REVERSE(default_rt);
	}

	DROP_STACK_TO_POLYMORPHIC();

	break;
}
PARSE_OP(OP_RETURN)
	if(param->current_func != NULL) {
		functiontype_t* ftype = &array_at(param->mod->types, param->current_func->typeidx);
		uint16_t r1 = 0;
		if(ftype->rt2.len > 0) {
			r1 = (uint16_t)(param->total_locals + type_stack->idx - ftype->rt2.len + 1);
		}
		POP_RESULTTYPE_REVERSE(ftype->rt2);
		param->instr->r1 = r1;
	}
	DROP_STACK_TO_POLYMORPHIC();

	break;
/* WASM 3.0 Exception Handling:
 * parser は引数の読み取りと検証のみを担い、実行 semantics は core エンジン側で処理する。 */
PARSE_OP(OP_THROW) {
	/* WASM 3.0 EH: tag の params をスタックから pop して例外として投擲する。
	 * src_reg_base = stack 上で param 列の先頭を指す register を r1 に格納する。 */
	param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
	uint32_t tagidx;
	_throwiferr(read_u32_leb(&tagidx, param->buf));
	uint32_t total_tags = param->mod->tagimport_count + (uint32_t)param->mod->tags.len;
	_throwif(ERR_MALFORMED_FUNC, tagidx >= total_tags);
	instr_ex->val = tagidx;
	uint32_t typeidx = UINT32_MAX;
	if(tagidx < param->mod->tagimport_count) {
		uint32_t imp_count = 0;
		foreach(im, import_t, param->mod->imports) {
			if(im->d.kind == IMPORTDESC_TAG) {
				if(imp_count == tagidx) {
					typeidx = im->d.tag.typeidx;
					break;
				}
				imp_count++;
			}
		}
	} else {
		uint32_t local_idx = tagidx - param->mod->tagimport_count;
		if(local_idx < param->mod->tags.len)
			typeidx = param->mod->tags.data[local_idx].tagtype.typeidx;
	}
	_throwif(ERR_MALFORMED_FUNC, typeidx >= param->mod->types.len);
	{
		functiontype_t* tag_ft = &param->mod->types.data[typeidx];
		uint16_t r1 = 0;
		if(tag_ft->rt1.len > 0)
			r1 = (uint16_t)(param->total_locals + type_stack->idx - (int32_t)tag_ft->rt1.len + 1);
		POP_RESULTTYPE_REVERSE(tag_ft->rt1);
		param->instr->r1 = r1;
	}
	DROP_STACK_TO_POLYMORPHIC();
	break;
}
PARSE_OP(OP_RETHROW) {
	/* WASM 3.0 EH: rethrow L は L 番目の surrounding label が catch arm の
	 * try-block を指していないと invalid。kw_parser_stack を辿って is_catch_arm
	 * を確認する。 */
	uint32_t labelidx;
	_throwiferr(read_u32_leb(&labelidx, param->buf));
	_throwif(ERR_MALFORMED_BLOCK, labelidx >= param->label_depth);
	{
		parser_stack_t* target = param->current_block;
		for(uint32_t i = 0; i < labelidx; i++)
			target = kinowasm_stack_entry(target->link.prev, parser_stack_t, link);
		_throwif(ERR_MALFORMED_BLOCK, target == param->root_block || !target->is_catch_arm);
	}
	DROP_STACK_TO_POLYMORPHIC();
	break;
}
PARSE_OP(OP_DELEGATE) {
	/* legacy try-delegate: 直近の OP_TRY を OP_TRY_DELEGATE に patch するため
	 * labelidx は instr->r1/r2 に保存。block-end 検出は MODE_BLOCK_LOOP で行う。
	 * delegate L は try の外側の L 番目 surrounding label を指すため、try 自身を
	 * 含む label_depth から 1 引いた範囲のみ有効 (L >= label_depth-1 は invalid)。 */
	uint32_t labelidx;
	_throwiferr(read_u32_leb(&labelidx, param->buf));
	_throwif(ERR_MALFORMED_BLOCK, param->label_depth == 0 || labelidx >= param->label_depth - 1);
	param->instr->r1 = (uint16_t)(labelidx & 0xFFFF);
	param->instr->r2 = (uint16_t)((labelidx >> 16) & 0xFFFF);
	/* try body の結果型検証 (OP_END 相当)。drop 前でないと polymorphic 緩和で素通りする。 */
	_throwif(ERR_MALFORMED_BLOCK, param->current_block->is_catch_arm);
	_throwiferr(kw_parser_validate_try_section_end(param));
	DROP_STACK_TO_POLYMORPHIC();
	break;
}
PARSE_OP(OP_CATCH) {
	/* WASM 3.0 EH: tagidx を r1/r2 にパック (16+16=32bit)。post-decode loop が
	 * これを読んで tag の param 型を type stack へ push する。 */
	uint32_t tagidx;
	_throwiferr(read_u32_leb(&tagidx, param->buf));
	uint32_t total_tags = param->mod->tagimport_count + (uint32_t)param->mod->tags.len;
	_throwif(ERR_MALFORMED_FUNC, tagidx >= total_tags);
	param->instr->r1 = (uint16_t)(tagidx & 0xFFFF);
	param->instr->r2 = (uint16_t)((tagidx >> 16) & 0xFFFF);
	/* body / 直前 handler の結果型検証 (drop 前でないと polymorphic 緩和で素通りする)。 */
	_throwiferr(kw_parser_validate_try_section_end(param));
	DROP_STACK_TO_POLYMORPHIC();
	break;
}
PARSE_OP(OP_CATCH_ALL) {
	param->instr->r1 = 0;
	param->instr->r2 = 0;
	_throwiferr(kw_parser_validate_try_section_end(param));
	DROP_STACK_TO_POLYMORPHIC();
	break;
}
PARSE_OP(OP_THROW_REF) {
	/* throw_ref は exnref を 1 個 pop する。検証を省くと空スタック / 型不一致の throw_ref を
	 * 受理し、core (case 0x0A) が無条件 to_slot(cdepth-1) で st[-1] を OOB read して AV する。
	 * pop 後に stack polymorphic 化 (throw_ref は制御フローを終端する)。 */
	UNUSE uint16_t exnref_reg;
	POP_REG(exnref_reg, TYPE_EXNREF);
	DROP_STACK_TO_POLYMORPHIC();
	break;
}
PARSE_OP(OP_TRY_TABLE) {
	/* WASM 3.0 EH try_table:
	 * blocktype + handlers list + body + end の構造。handlers は core が raw bytes から
	 * 再デコードするため保持せず、ここでは cflag / tagidx / labelidx の検証のみ行う。 */
	param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
	_throwiferr(read_blocktype(&instr_ex->blocktype, param->buf));
	uint32_t catch_n;
	_throwiferr(read_u32_leb(&catch_n, param->buf));
	uint32_t total_tags = param->mod->tagimport_count + (uint32_t)param->mod->tags.len;
	/* 悪意ある巨大 catch_n で不当に長いループを回さないよう先に弾く。
	 * 各 handler は最低 2 byte (cflag + labelidx LEB) なので、残バッファ size が上限。 */
	_throwif(ERR_MALFORMED_FUNC,
		(uint64_t)catch_n > param->buf->len - param->buf->cur);
	for(uint32_t i = 0; i < catch_n; i++) {
		uint8_t cflag;
		_throwiferr(read_u8(&cflag, param->buf));
		_throwif(ERR_MALFORMED_FUNC, cflag > 3);
		/* WASM 3.0 EH 新仕様 try_table の handler 形式 (binary spec):
		 *   0x00 catch       tagidx labelidx
		 *   0x01 catch_ref   tagidx labelidx
		 *   0x02 catch_all          labelidx
		 *   0x03 catch_all_ref      labelidx
		 * tagidx を読むのは cflag 0/1。catch_ref が 1 で、catch_all が 2 である点に注意。 */
		uint32_t tagidx = 0;
		if(cflag == 0 || cflag == 1) {
			_throwiferr(read_u32_leb(&tagidx, param->buf));
			_throwif(ERR_MALFORMED_FUNC, tagidx >= total_tags);
		}
		uint32_t labelidx;
		_throwiferr(read_u32_leb(&labelidx, param->buf));
		_throwif(ERR_MALFORMED_BLOCK, labelidx >= param->label_depth);
		_throwiferr(kw_parser_validate_catch(param, cflag, tagidx, labelidx));
	}
	/* BLOCK と同様に block ボディを処理 */
	{
		functiontype_t blk_ft;
		uint8_t blk_vt_buf[2];
		_throwiferr(kw_parser_resolve_blocktype(&blk_ft, instr_ex->blocktype, param->mod, blk_vt_buf));
		POP_RESULTTYPE_REVERSE(blk_ft.rt1);
		param->instr->r0 = (uint16_t)(param->total_locals + type_stack->idx + 1);
	}
	break;
}
PARSE_OP(OP_CALL) {
	uint32_t funcidx;
	param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
	_throwiferr(read_u32_leb(&funcidx, param->buf));
	_throwif(ERR_MALFORMED_FUNC, funcidx >= param->total_funcs);
	instr_ex->val = funcidx;
	{
		functiontype_t* ft = &array_at((*param->funcs), funcidx);
		uint16_t r1 = 0;
		if(ft->rt1.len > 0) {
			r1 = (uint16_t)(param->total_locals + type_stack->idx - ft->rt1.len + 1);
		}
		POP_RESULTTYPE_REVERSE(ft->rt1);
		uint16_t r0 = 0;
		if(ft->rt2.len > 0) {
			r0 = (uint16_t)(param->total_locals + type_stack->idx + 1);
		}
		PUSH_RESULTTYPE(ft->rt2);
		param->instr->r0 = r0;
		param->instr->r1 = r1;
	}
	break;
}
/* WASM 3.0 tail call: 被呼関数の戻り値型は現関数の戻り値型と一致が必要。
 * 戻り値はそのまま現関数の caller に流れるため type stack に push せず polymorphic 化。 */
PARSE_OP(OP_RETURN_CALL) {
	uint32_t funcidx;
	param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
	_throwiferr(read_u32_leb(&funcidx, param->buf));
	_throwif(ERR_MALFORMED_FUNC, funcidx >= param->total_funcs);
	instr_ex->val = funcidx;
	{
		functiontype_t* ft = &array_at((*param->funcs), funcidx);
		/* 被呼関数の戻り値型 == 現関数の戻り値型をチェック */
		if(param->current_func != NULL) {
			functiontype_t* cur_ft = &array_at(param->mod->types, param->current_func->typeidx);
			_throwif(ERR_TYPE_MISMATCH, ft->rt2.len != cur_ft->rt2.len);
			_throwif(ERR_TYPE_MISMATCH, ft->rt2.len > 0 && memcmp(ft->rt2.data, cur_ft->rt2.data, ft->rt2.len) != 0);
		}
		uint16_t r1 = 0;
		if(ft->rt1.len > 0) {
			r1 = (uint16_t)(param->total_locals + type_stack->idx - ft->rt1.len + 1);
		}
		POP_RESULTTYPE_REVERSE(ft->rt1);
		param->instr->r1 = r1;
	}
	DROP_STACK_TO_POLYMORPHIC();
	break;
}
PARSE_OP(OP_CALL_INDIRECT) {
	param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
	_throwiferr(read_u32_leb(&instr_ex->y, param->buf));
	_throwiferr(read_u32_leb(&instr_ex->x, param->buf));
	_throwif(ERR_MALFORMED_FUNC, instr_ex->x >= param->total_tables);
	_throwif(ERR_MALFORMED_FUNC, instr_ex->y >= param->total_types);
	uint8_t idx_type;
	{
		tabletype_t* tt = &array_at((*param->tables), instr_ex->x);
		if(tt->reftype != TYPE_FUNCREF)
			_throw(ERR_TYPE_MISMATCH);

		idx_type = tt->limits.is_64 ? TYPE_VAL_I64 : TYPE_VAL_I32;
	}
	uint16_t r2;
	POP_REG(r2, idx_type);
	{
		functiontype_t* ft = &array_at(param->mod->types, instr_ex->y);
		uint16_t r1 = 0;
		if(ft->rt1.len > 0) {
			r1 = (uint16_t)(param->total_locals + type_stack->idx - ft->rt1.len + 1);
		}
		POP_RESULTTYPE_REVERSE(ft->rt1);
		uint16_t r0 = 0;
		if(ft->rt2.len > 0) {
			r0 = (uint16_t)(param->total_locals + type_stack->idx + 1);
		}
		PUSH_RESULTTYPE(ft->rt2);
		param->instr->r0 = r0;
		param->instr->r1 = r1;
		param->instr->r2 = r2;
	}
	break;
}
/* WASM 3.0 tail call indirect: call_indirect + return の型整合 */
PARSE_OP(OP_RETURN_CALL_INDIRECT) {
	param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
	_throwiferr(read_u32_leb(&instr_ex->y, param->buf));
	_throwiferr(read_u32_leb(&instr_ex->x, param->buf));
	_throwif(ERR_MALFORMED_FUNC, instr_ex->x >= param->total_tables);
	_throwif(ERR_MALFORMED_FUNC, instr_ex->y >= param->total_types);
	uint8_t idx_type;
	{
		tabletype_t* tt = &array_at((*param->tables), instr_ex->x);
		if(tt->reftype != TYPE_FUNCREF)
			_throw(ERR_TYPE_MISMATCH);

		idx_type = tt->limits.is_64 ? TYPE_VAL_I64 : TYPE_VAL_I32;
	}
	uint16_t r2;
	POP_REG(r2, idx_type);
	{
		functiontype_t* ft = &array_at(param->mod->types, instr_ex->y);
		if(param->current_func != NULL) {
			functiontype_t* cur_ft = &array_at(param->mod->types, param->current_func->typeidx);
			_throwif(ERR_TYPE_MISMATCH, ft->rt2.len != cur_ft->rt2.len);
			_throwif(ERR_TYPE_MISMATCH, ft->rt2.len > 0 && memcmp(ft->rt2.data, cur_ft->rt2.data, ft->rt2.len) != 0);
		}
		uint16_t r1 = 0;
		if(ft->rt1.len > 0) {
			r1 = (uint16_t)(param->total_locals + type_stack->idx - ft->rt1.len + 1);
		}
		POP_RESULTTYPE_REVERSE(ft->rt1);
		param->instr->r1 = r1;
		param->instr->r2 = r2;
	}
	DROP_STACK_TO_POLYMORPHIC();
	break;
}
PARSE_OP(OP_DROP) {
	/* polymorphic drop: V128 の場合は 2 entry pop する。 */
	uint8_t t = 0;
	type_stack_peek(type_stack, param->type_stack_limit_idx, &t);
	UNUSE uint16_t r1;
	POP_REG(r1, 0);
	if(t == TYPE_VAL_V128) {
		POP_REG(r1, 0);
	}
	break;
}
PARSE_OP(OP_SELECT) {
	uint16_t r3, r2, r1, r0;
	POP_REG(r3, TYPE_VAL_I32);
	{
		uint8_t t;
		_throwif(ERR_TYPE_MISMATCH, type_stack_peek(type_stack, param->type_stack_limit_idx, &t));
		if(kw_parser_is_reftype(t))
			_throw(ERR_TYPE_MISMATCH);

		if(t == TYPE_VAL_V128) {
			POP_REG_V128(r2);
			POP_REG_V128(r1);
			PUSH_REG_V128(r0);
			SET_SELECT_INSTR(r0, r1, r2, r3);
			param->instr->opcode = OP_SELECT_V128;
			break;
		}
		POP_REG(r2, t);
		POP_REG(r1, t);
		PUSH_REG(r0, t);
	}
	SET_SELECT_INSTR(r0, r1, r2, r3);
	break;
}
PARSE_OP(OP_SELECT_T) {
	uint32_t type_size;
	_throwiferr(read_u32_leb(&type_size, param->buf));
	/* #24: 仕様上 typed select の result type vector 長は厳密に 1。type_size==0
	 * (unknown-type marker 混入) や >1 (silent truncation) を弾く。 */
	_throwif(ERR_TYPE_MISMATCH, type_size != 1);
	uint8_t t = 0;
	for(uint32_t i = 0; i < type_size; i++) {
		uint8_t type;
		_throwiferr(read_u8(&type, param->buf));
		/* 型 vec の要素は valtype でなければならない。特に 0x00 は type_stack_pop の
		 * 「任意適合」marker と衝突し、型検査なし select が通ってしまうため必ず弾く。 */
		_throwif(ERR_TYPE_MISMATCH, !kw_parser_is_valtype(type));
		if(i == 0)
			t = type;
	}
#if !KINOWASM_ENABLE_SIMD
	_throwif(ERR_FEATURE_DISABLED, t == TYPE_VAL_V128);
#endif

	uint16_t r3, r2, r1, r0;
	POP_REG(r3, TYPE_VAL_I32);
	if(t == TYPE_VAL_V128) {
		POP_REG_V128(r2);
		POP_REG_V128(r1);
		PUSH_REG_V128(r0);
		SET_SELECT_INSTR(r0, r1, r2, r3);
		param->instr->opcode = OP_SELECT_V128;
		break;
	}
	POP_REG(r2, t);
	POP_REG(r1, t);
	PUSH_REG(r0, t);

	SET_SELECT_INSTR(r0, r1, r2, r3);
	break;
}
/* kw_parser_op_core.inc — kw_parser_code_instr の命令デコード switch の一部 (変数操作 / テーブル / メモリ Load・Store / 定数)。
 * parser.c の BEGIN_PARSE ... END_PARSE の間に #include される。
 * 各 case は PARSE_OP(op) マクロ (= `case op:`) で始まる。単体では
 * コンパイルできない (parser.c の switch / マクロ文脈に依存)。 */
/* --- 変数操作 --- */
PARSE_OP(OP_LOCAL_GET) {
	uint32_t localidx;
	_throwiferr(read_u32_leb(&localidx, param->buf));
	_throwif(ERR_MALFORMED_LOCALVAL, localidx >= param->localvalues->len);
	if(param->current_func) {
		uint8_t local_type = array_at((*param->localvalues), localidx);
		uint16_t local_slot = (uint16_t)array_at((*param->local_offsets), localidx);
		uint16_t r0;
		if(local_type == TYPE_VAL_V128) {
			PUSH_REG_V128(r0);
			param->instr->opcode = OP_COPY_V128;
		} else {
			PUSH_REG(r0, local_type);
			param->instr->opcode = OP_COPY;
		}
		param->instr->r0 = r0;
		param->instr->r1 = local_slot;
	}
	break;
}
PARSE_OP(OP_LOCAL_SET)
	PARSE_OP(OP_LOCAL_TEE) {
	uint32_t localidx;
	_throwiferr(read_u32_leb(&localidx, param->buf));
	_throwif(ERR_MALFORMED_LOCALVAL, localidx >= param->localvalues->len);
	if(param->current_func) {
		uint8_t local_type = array_at((*param->localvalues), localidx);
		uint16_t local_slot = (uint16_t)array_at((*param->local_offsets), localidx);
		uint16_t r1;
		if(local_type == TYPE_VAL_V128) {
			POP_REG_V128(r1);
			if(param->instr->opcode == OP_LOCAL_TEE) {
				UNUSE uint16_t r0;
				PUSH_REG_V128(r0);
			}
			param->instr->opcode = OP_COPY_V128;
			param->instr->r0 = local_slot;
			param->instr->r1 = r1;
		} else {
			POP_REG(r1, local_type);
			if(param->instr->opcode == OP_LOCAL_TEE) {
				UNUSE uint16_t r0;
				PUSH_REG(r0, local_type);
			}
			param->instr->opcode = OP_COPY;
			param->instr->r0 = local_slot;
			param->instr->r1 = r1;
		}
	}
	break;
}
PARSE_OP(OP_GLOBAL_GET) {
	uint32_t globalidx;
	globaltype_t* g;
	param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
	_throwiferr(read_u32_leb(&globalidx, param->buf));
	_throwif(ERR_MALFORMED_GLOBAL, globalidx >= param->total_globals);
	if(param->current_func == NULL)
		_throwif(ERR_MALFORMED_GLOBAL, !kw_parser_is_constexpr_global(param->mod, globalidx));
	g = kw_parser_get_globaltype(param->mod, globalidx);
	_throwif(ERR_MALFORMED_GLOBAL, g == NULL);
	uint16_t r0;
	if(g->valtype == TYPE_VAL_V128) {
		PUSH_REG_V128(r0);
		param->instr->opcode = OP_GLOBAL_GET_V128;
	} else {
		PUSH_REG(r0, g->valtype);
	}
	param->instr->r0 = r0;
	instr_ex->val = globalidx;
	break;
}
PARSE_OP(OP_GLOBAL_SET) {
	uint32_t globalidx;
	globaltype_t* g;
	param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
	_throwiferr(read_u32_leb(&globalidx, param->buf));
	_throwif(ERR_MALFORMED_GLOBAL, globalidx >= param->total_globals);
	g = kw_parser_get_globaltype(param->mod, globalidx);
	_throwif(ERR_MALFORMED_GLOBAL, g == NULL);
	_throwif(ERR_TYPE_MISMATCH, g->mut == 0);
	uint16_t r1;
	if(g->valtype == TYPE_VAL_V128) {
		POP_REG_V128(r1);
		param->instr->opcode = OP_GLOBAL_SET_V128;
	} else {
		POP_REG(r1, g->valtype);
	}
	param->instr->r1 = r1;
	instr_ex->val = globalidx;
	break;
}
/* --- テーブル --- */
PARSE_OP(OP_TABLE_GET) {
	tabletype_t* table_type;
	param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
	_throwiferr(read_u32_leb(&instr_ex->x, param->buf));
	_throwif(ERR_MALFORMED_TABLE, instr_ex->x >= param->total_tables);
	table_type = &array_at((*param->tables), instr_ex->x);
	/* WASM 3.0 table64: 64bit table の場合 index は i64。 */
	uint8_t idx_type = table_type->limits.is_64 ? TYPE_VAL_I64 : TYPE_VAL_I32;
	uint16_t r1, r0;
	POP_REG(r1, idx_type);
	PUSH_REG(r0, table_type->reftype);
	param->instr->r0 = r0;
	param->instr->r1 = r1;
	break;
}
PARSE_OP(OP_TABLE_SET) {
	tabletype_t* table_type;
	param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
	_throwiferr(read_u32_leb(&instr_ex->x, param->buf));
	_throwif(ERR_MALFORMED_TABLE, instr_ex->x >= param->total_tables);
	table_type = &array_at((*param->tables), instr_ex->x);
	uint8_t idx_type = table_type->limits.is_64 ? TYPE_VAL_I64 : TYPE_VAL_I32;
	uint16_t r2, r1;
	POP_REG(r2, table_type->reftype);
	POP_REG(r1, idx_type);
	param->instr->r1 = r1;
	param->instr->r2 = r2;
	break;
}
/* --- メモリ操作 (Load/Store) --- */
PARSE_OP(OP_I32_LOAD)
PARSE_OP(OP_I64_LOAD)
PARSE_OP(OP_F32_LOAD)
PARSE_OP(OP_F64_LOAD)
PARSE_OP(OP_I32_LOAD8_S)
PARSE_OP(OP_I32_LOAD8_U)
PARSE_OP(OP_I32_LOAD16_S)
PARSE_OP(OP_I32_LOAD16_U)
PARSE_OP(OP_I64_LOAD8_S)
PARSE_OP(OP_I64_LOAD8_U)
PARSE_OP(OP_I64_LOAD16_S)
PARSE_OP(OP_I64_LOAD16_U)
PARSE_OP(OP_I64_LOAD32_S)
PARSE_OP(OP_I64_LOAD32_U) {
	param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
	_throwif(ERR_MALFORMED_STORE, param->total_memories == 0);
	uint64_t _off64;
	uint32_t _memidx32;
	READ_MEMARG_EX(instr_ex->memory, _off64, _memidx32);

	/* memarg.align は log2(alignment)。natural alignment (最大 8 byte) を超える
	 * 値は invalid。align >= 32 は (1u << align) が overflow するので先に弾く。 */
	_throwif(ERR_MALFORMED_ALIGN, instr_ex->memory.align >= 32 || (1u << instr_ex->memory.align) > operand_type_size(param->instr->opcode));

	/* WASM 3.0 Memory64: 対象 memory が is_64 ならアドレス operand は i64。
	 * parser で OP_MEM64_LOAD に rewrite し、元 op を ipex->memory.align に退避する。 */
	const memorytype_t* _mt = get_memtype_by_idx(param->mod, _memidx32);
	uint8_t _addr_type = (_mt && _mt->is_64) ? TYPE_VAL_I64 : TYPE_VAL_I32;
	/* memory32 の offset は u32 範囲まで (u64 offset は memory64 のみ有効)。 */
	_throwif(ERR_MALFORMED_FUNC, _addr_type == TYPE_VAL_I32 && _off64 > 0xFFFFFFFFull);

	uint16_t r1;
	POP_REG(r1, _addr_type);
	uint16_t r0;
	PUSH_REG(r0, operand_value_type(param->instr->opcode));
	param->instr->r0 = r0;
	param->instr->r1 = r1;
	if(_addr_type == TYPE_VAL_I64) {
		/* memory64: 元 op を align に退避してから rewrite。 */
		instr_ex->memory.align = (uint16_t)param->instr->opcode;
		param->instr->opcode = OP_MEM64_LOAD;
	}
	break;
}
PARSE_OP(OP_I32_STORE)
PARSE_OP(OP_I64_STORE)
PARSE_OP(OP_F32_STORE)
PARSE_OP(OP_F64_STORE)
PARSE_OP(OP_I32_STORE8)
PARSE_OP(OP_I32_STORE16)
PARSE_OP(OP_I64_STORE8)
PARSE_OP(OP_I64_STORE16)
PARSE_OP(OP_I64_STORE32) {
	param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
	_throwif(ERR_MALFORMED_STORE, param->total_memories == 0);
	uint64_t _off64;
	uint32_t _memidx32;
	READ_MEMARG_EX(instr_ex->memory, _off64, _memidx32);
	/* memarg.align は log2(alignment)。natural alignment (最大 8 byte) を超える
	 * 値は invalid。align >= 32 は (1u << align) が overflow するので先に弾く。 */
	_throwif(ERR_MALFORMED_ALIGN, instr_ex->memory.align >= 32 || (1u << instr_ex->memory.align) > operand_type_size(param->instr->opcode));

	/* WASM 3.0 Memory64: 対象 memory が is_64 ならアドレス operand は i64。 */
	const memorytype_t* _mt = get_memtype_by_idx(param->mod, _memidx32);
	uint8_t _addr_type = (_mt && _mt->is_64) ? TYPE_VAL_I64 : TYPE_VAL_I32;
	/* memory32 の offset は u32 範囲まで (u64 offset は memory64 のみ有効)。 */
	_throwif(ERR_MALFORMED_FUNC, _addr_type == TYPE_VAL_I32 && _off64 > 0xFFFFFFFFull);

	uint16_t r2, r1;
	POP_REG(r2, operand_value_type(param->instr->opcode));
	POP_REG(r1, _addr_type);
	param->instr->r1 = r1;
	param->instr->r2 = r2;
	if(_addr_type == TYPE_VAL_I64) {
		instr_ex->memory.align = (uint16_t)param->instr->opcode;
		param->instr->opcode = OP_MEM64_STORE;
	}
	break;
}
PARSE_OP(OP_MEMORY_SIZE) {
	/* WASM 3.0 multi-memory + memory64: memidx を u32 leb で読み、
	 * memtype の is_64 に応じて push 型 (i32 / i64) を切り替える。 */
	uint32_t memidx;
	_throwiferr(read_u32_leb(&memidx, param->buf));
	_throwif(ERR_MALFORMED_MEMORY, memidx >= param->total_memories);
	const memorytype_t* mt = get_memtype_by_idx(param->mod, memidx);
	uint8_t result_type = (mt && mt->is_64) ? TYPE_VAL_I64 : TYPE_VAL_I32;
	uint16_t r0;
	PUSH_REG(r0, result_type);
	param->instr->r0 = r0;
	param->instr->r1 = (uint16_t)memidx;
	break;
}
PARSE_OP(OP_MEMORY_GROW) {
	uint32_t memidx;
	_throwiferr(read_u32_leb(&memidx, param->buf));
	_throwif(ERR_MALFORMED_MEMORY, memidx >= param->total_memories);
	const memorytype_t* mt = get_memtype_by_idx(param->mod, memidx);
	uint8_t pages_type = (mt && mt->is_64) ? TYPE_VAL_I64 : TYPE_VAL_I32;
	uint16_t r1, r0;
	POP_REG(r1, pages_type);
	PUSH_REG(r0, pages_type);
	param->instr->r0 = r0;
	param->instr->r1 = r1;
	param->instr->r2 = (uint16_t)memidx;
	break;
}
/* --- 定数 --- */
PARSE_OP(OP_I32_CONST) {
	param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
	_throwiferr(read_s32_leb(&instr_ex->c.i32, param->buf));
	uint16_t r0;
	PUSH_REG(r0, TYPE_VAL_I32);
	param->instr->r0 = r0;
	break;
}
PARSE_OP(OP_I64_CONST) {
	param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
	_throwiferr(read_s64_leb(&instr_ex->c.i64, param->buf));
	uint16_t r0;
	PUSH_REG(r0, TYPE_VAL_I64);
	param->instr->r0 = r0;
	break;
}
PARSE_OP(OP_F32_CONST) {
	param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
	_throwiferr(read_f32(&instr_ex->c.f32, param->buf));
	uint16_t r0;
	PUSH_REG(r0, TYPE_VAL_F32);
	param->instr->r0 = r0;
	break;
}
PARSE_OP(OP_F64_CONST) {
	param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
	_throwiferr(read_f64(&instr_ex->c.f64, param->buf));
	uint16_t r0;
	PUSH_REG(r0, TYPE_VAL_F64);
	param->instr->r0 = r0;
	break;
}
/* kw_parser_op_numeric.inc — kw_parser_code_instr の命令デコード switch の一部 (数値演算 (i32/i64/f32/f64) / 参照命令)。
 * parser.c の BEGIN_PARSE ... END_PARSE の間に #include される。
 * 各 case は PARSE_OP(op) マクロ (= `case op:`) で始まる。単体では
 * コンパイルできない (parser.c の switch / マクロ文脈に依存)。 */
/* --- 数値演算 (抜粋: 他の演算も同様パターン) ---
 * i32 unary */
PARSE_OP(OP_I32_EQZ)
PARSE_OP(OP_I32_CLZ)
PARSE_OP(OP_I32_CTZ)
PARSE_OP(OP_I32_POPCNT)
PARSE_OP(OP_I32_EXTEND8_S)
PARSE_OP(OP_I32_EXTEND16_S) {
	uint16_t r1, r0;
	POP_REG(r1, TYPE_VAL_I32);
	PUSH_REG(r0, TYPE_VAL_I32);
	param->instr->r0 = r0;
	param->instr->r1 = r1;
	break;
}
/* i32 binary */
PARSE_OP(OP_I32_EQ)
PARSE_OP(OP_I32_NE)
PARSE_OP(OP_I32_LT_S)
PARSE_OP(OP_I32_LT_U)
PARSE_OP(OP_I32_GT_S)
PARSE_OP(OP_I32_GT_U)
PARSE_OP(OP_I32_LE_S)
PARSE_OP(OP_I32_LE_U)
PARSE_OP(OP_I32_GE_S)
PARSE_OP(OP_I32_GE_U)
PARSE_OP(OP_I32_ADD)
PARSE_OP(OP_I32_SUB)
PARSE_OP(OP_I32_MUL)
PARSE_OP(OP_I32_DIV_S)
PARSE_OP(OP_I32_DIV_U)
PARSE_OP(OP_I32_REM_S)
PARSE_OP(OP_I32_REM_U)
PARSE_OP(OP_I32_AND)
PARSE_OP(OP_I32_OR)
PARSE_OP(OP_I32_XOR)
PARSE_OP(OP_I32_SHL)
PARSE_OP(OP_I32_SHR_S)
PARSE_OP(OP_I32_SHR_U)
PARSE_OP(OP_I32_ROTL)
PARSE_OP(OP_I32_ROTR) {
	PARSE_BINARY(TYPE_VAL_I32, TYPE_VAL_I32);
	break;
}
/* i64 unary */
PARSE_OP(OP_I64_EQZ) {
	uint16_t r1, r0;
	POP_REG(r1, TYPE_VAL_I64);
	PUSH_REG(r0, TYPE_VAL_I32);
	param->instr->r0 = r0;
	param->instr->r1 = r1;
	break;
}
PARSE_OP(OP_I64_CLZ)
PARSE_OP(OP_I64_CTZ)
PARSE_OP(OP_I64_POPCNT)
PARSE_OP(OP_I64_EXTEND8_S)
PARSE_OP(OP_I64_EXTEND16_S)
PARSE_OP(OP_I64_EXTEND32_S) {
	uint16_t r1, r0;
	POP_REG(r1, TYPE_VAL_I64);
	PUSH_REG(r0, TYPE_VAL_I64);
	param->instr->r0 = r0;
	param->instr->r1 = r1;
	break;
}
/* i64 binary */
PARSE_OP(OP_I64_EQ)
PARSE_OP(OP_I64_NE)
PARSE_OP(OP_I64_LT_S)
PARSE_OP(OP_I64_LT_U)
PARSE_OP(OP_I64_GT_S)
PARSE_OP(OP_I64_GT_U)
PARSE_OP(OP_I64_LE_S)
PARSE_OP(OP_I64_LE_U)
PARSE_OP(OP_I64_GE_S)
PARSE_OP(OP_I64_GE_U) {
	PARSE_BINARY(TYPE_VAL_I64, TYPE_VAL_I32);
	break;
}
PARSE_OP(OP_I64_ADD)
PARSE_OP(OP_I64_SUB)
PARSE_OP(OP_I64_MUL)
PARSE_OP(OP_I64_DIV_S)
PARSE_OP(OP_I64_DIV_U)
PARSE_OP(OP_I64_REM_S)
PARSE_OP(OP_I64_REM_U)
PARSE_OP(OP_I64_AND)
PARSE_OP(OP_I64_OR)
PARSE_OP(OP_I64_XOR)
PARSE_OP(OP_I64_SHL)
PARSE_OP(OP_I64_SHR_S)
PARSE_OP(OP_I64_SHR_U)
PARSE_OP(OP_I64_ROTL)
PARSE_OP(OP_I64_ROTR) {
	PARSE_BINARY(TYPE_VAL_I64, TYPE_VAL_I64);
	break;
}
/* f32 ops */
PARSE_OP(OP_F32_EQ)
PARSE_OP(OP_F32_NE)
PARSE_OP(OP_F32_LT)
PARSE_OP(OP_F32_GT)
PARSE_OP(OP_F32_LE)
PARSE_OP(OP_F32_GE) {
PARSE_BINARY(TYPE_VAL_F32, TYPE_VAL_I32);
	break;
}
PARSE_OP(OP_F32_ABS)
PARSE_OP(OP_F32_NEG)
PARSE_OP(OP_F32_CEIL)
PARSE_OP(OP_F32_FLOOR)
PARSE_OP(OP_F32_TRUNC)
PARSE_OP(OP_F32_NEAREST)
PARSE_OP(OP_F32_SQRT) {
PARSE_UNARY(TYPE_VAL_F32, TYPE_VAL_F32);
	break;
}
PARSE_OP(OP_F32_ADD)
PARSE_OP(OP_F32_SUB)
PARSE_OP(OP_F32_MUL)
PARSE_OP(OP_F32_DIV)
PARSE_OP(OP_F32_MIN)
PARSE_OP(OP_F32_MAX)
PARSE_OP(OP_F32_COPYSIGN) {
PARSE_BINARY(TYPE_VAL_F32, TYPE_VAL_F32);
	break;
}
/* f64 ops */
PARSE_OP(OP_F64_EQ)
PARSE_OP(OP_F64_NE)
PARSE_OP(OP_F64_LT)
PARSE_OP(OP_F64_GT)
PARSE_OP(OP_F64_LE)
PARSE_OP(OP_F64_GE) {
PARSE_BINARY(TYPE_VAL_F64, TYPE_VAL_I32);
	break;
}
PARSE_OP(OP_F64_ABS)
PARSE_OP(OP_F64_NEG)
PARSE_OP(OP_F64_CEIL)
PARSE_OP(OP_F64_FLOOR)
PARSE_OP(OP_F64_TRUNC)
PARSE_OP(OP_F64_NEAREST)
PARSE_OP(OP_F64_SQRT) {
PARSE_UNARY(TYPE_VAL_F64, TYPE_VAL_F64);
	break;
}
PARSE_OP(OP_F64_ADD)
PARSE_OP(OP_F64_SUB)
PARSE_OP(OP_F64_MUL)
PARSE_OP(OP_F64_DIV)
PARSE_OP(OP_F64_MIN)
PARSE_OP(OP_F64_MAX)
PARSE_OP(OP_F64_COPYSIGN) {
PARSE_BINARY(TYPE_VAL_F64, TYPE_VAL_F64);
	break;
}
/* 型変換オペコード (0xA7-0xBF): テーブル駆動で統一処理 */
PARSE_OP(OP_I32_WRAP_I64)
PARSE_OP(OP_I32_TRUNC_F32_S) PARSE_OP(OP_I32_TRUNC_F32_U)
PARSE_OP(OP_I32_TRUNC_F64_S) PARSE_OP(OP_I32_TRUNC_F64_U)
PARSE_OP(OP_I64_EXTEND_I32_S) PARSE_OP(OP_I64_EXTEND_I32_U)
PARSE_OP(OP_I64_TRUNC_F32_S) PARSE_OP(OP_I64_TRUNC_F32_U)
PARSE_OP(OP_I64_TRUNC_F64_S) PARSE_OP(OP_I64_TRUNC_F64_U)
PARSE_OP(OP_F32_CONVERT_I32_S) PARSE_OP(OP_F32_CONVERT_I32_U)
PARSE_OP(OP_F32_CONVERT_I64_S) PARSE_OP(OP_F32_CONVERT_I64_U)
PARSE_OP(OP_F32_DEMOTE_F64)
PARSE_OP(OP_F64_CONVERT_I32_S) PARSE_OP(OP_F64_CONVERT_I32_U)
PARSE_OP(OP_F64_CONVERT_I64_S) PARSE_OP(OP_F64_CONVERT_I64_U)
PARSE_OP(OP_F64_PROMOTE_F32)
PARSE_OP(OP_I32_REINTERPRET_F32) PARSE_OP(OP_I64_REINTERPRET_F64)
PARSE_OP(OP_F32_REINTERPRET_I32) PARSE_OP(OP_F64_REINTERPRET_I64) {
	static const uint8_t conv_types[][2] = {
		{TYPE_VAL_I64,TYPE_VAL_I32}, {TYPE_VAL_F32,TYPE_VAL_I32}, {TYPE_VAL_F32,TYPE_VAL_I32},
		{TYPE_VAL_F64,TYPE_VAL_I32}, {TYPE_VAL_F64,TYPE_VAL_I32}, {TYPE_VAL_I32,TYPE_VAL_I64},
		{TYPE_VAL_I32,TYPE_VAL_I64}, {TYPE_VAL_F32,TYPE_VAL_I64}, {TYPE_VAL_F32,TYPE_VAL_I64},
		{TYPE_VAL_F64,TYPE_VAL_I64}, {TYPE_VAL_F64,TYPE_VAL_I64}, {TYPE_VAL_I32,TYPE_VAL_F32},
		{TYPE_VAL_I32,TYPE_VAL_F32}, {TYPE_VAL_I64,TYPE_VAL_F32}, {TYPE_VAL_I64,TYPE_VAL_F32},
		{TYPE_VAL_F64,TYPE_VAL_F32}, {TYPE_VAL_I32,TYPE_VAL_F64}, {TYPE_VAL_I32,TYPE_VAL_F64},
		{TYPE_VAL_I64,TYPE_VAL_F64}, {TYPE_VAL_I64,TYPE_VAL_F64}, {TYPE_VAL_F32,TYPE_VAL_F64},
		{TYPE_VAL_F32,TYPE_VAL_I32}, {TYPE_VAL_F64,TYPE_VAL_I64}, {TYPE_VAL_I32,TYPE_VAL_F32},
		{TYPE_VAL_I64,TYPE_VAL_F64},
	};
	uint32_t idx = param->instr->opcode - OP_I32_WRAP_I64;
	PARSE_UNARY(conv_types[idx][0], conv_types[idx][1]);
	break;
}
/* --- 参照命令 --- */
PARSE_OP(OP_REF_NULL) {
	uint8_t t;
	_throwiferr(read_u8(&t, param->buf));
	/* heaptype は funcref / externref / exnref のみ受理する。core (kw_core_compile.c の
	 * 0xd0) は heaptype を SLEB (rds64) で読み進めるため、パーサが 1 byte 固定で読むと
	 * 継続ビット付きの不正な heaptype (typed ref.null の typeidx 符号化を含む) で
	 * パーサと core の命令ストリーム境界が desync し、core が未検証の後続バイトを
	 * オペコード/オペランドとして誤読する。有効な reftype に限定して弾く。 */
	_throwif(ERR_MALFORMED_REFERENCE_TYPE, t != TYPE_FUNCREF && t != TYPE_EXTERNREF && t != TYPE_EXNREF);
	uint16_t r0;
	PUSH_REG(r0, t);
	param->instr->r0 = r0;
	instr_ex->val = t;
	break;
}
PARSE_OP(OP_REF_IS_NULL) {
	uint8_t type;
	_throwif(ERR_TYPE_MISMATCH, type_stack_peek(type_stack, param->type_stack_limit_idx, &type));
	_throwif(ERR_TYPE_MISMATCH, type != 0 && !kw_parser_is_reftype(type));
	uint16_t r1, r0;
	POP_REG(r1, 0);
	PUSH_REG(r0, TYPE_VAL_I32);
	param->instr->r0 = r0;
	param->instr->r1 = r1;
	break;
}
PARSE_OP(OP_REF_FUNC) {
	param->instr_flag |= INSTRFLAG_USE_INSTR_EX;
	_throwiferr(read_u32_leb(&instr_ex->x, param->buf));
	_throwif(ERR_MALFORMED_FUNC, instr_ex->x >= param->total_funcs);
	if(param->current_func == NULL)
		_throwiferr(kw_parser_mark_declared_func(param->mod, instr_ex->x));
	else
		_throwif(ERR_MALFORMED_REFERENCE_TYPE, !kw_parser_is_declared_func(param->mod, instr_ex->x));

	uint16_t r0;
	PUSH_REG(r0, TYPE_FUNCREF);
	param->instr->r0 = r0;
	break;
}
