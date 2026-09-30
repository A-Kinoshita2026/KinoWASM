cmake_minimum_required(VERSION 4.2)

add_executable(testsuite_runner
	testsuite_runner.c
  ../host/systemmemory.c
	../host/winapi.c
	../host/extrafunction.c
	../host/debug.c
)

target_include_directories(testsuite_runner PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/../host"
	"${CMAKE_CURRENT_SOURCE_DIR}/../KinoWASM"
	"${CMAKE_CURRENT_SOURCE_DIR}/../KinoWASM/store"
	"${CMAKE_CURRENT_SOURCE_DIR}/../KinoWASM/KinoUtil"
)

target_link_libraries(testsuite_runner PRIVATE KinoWASM)

add_test(NAME testsuite_runner COMMAND testsuite_runner)
set_tests_properties(testsuite_runner PROPERTIES
	WORKING_DIRECTORY "${TARGET_BINARY_DIR}"
	FAIL_REGULAR_EXPRESSION "failed=[1-9]")

add_custom_command(
  TARGET testsuite_runner POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE_NAME:testsuite_runner> "${TARGET_BINARY_DIR}/"
)

# wast2json は wabt 同梱。見つからない場合は警告のみ (テストは未生成扱い)。
find_program(WAST2JSON_EXECUTABLE wast2json
  HINTS "D:/Tools/wabt/bin"
  DOC "wast2json from wabt for building test json files")

if(WAST2JSON_EXECUTABLE)
  # 単一の wast を wast2json で .json + .N.wasm に変換するマクロ。
  # OUT_DIR_NAME を空にすると ${TARGET_BINARY_DIR}/Test/testsuite 直下に配置される。
  # 第 4 引数以降 (ARGN) は wast2json への追加フラグ (例: --enable-exceptions)。
  function(_kw_add_wast2json TARGET_NAME WAST_FILE OUT_DIR_NAME)
    if(OUT_DIR_NAME STREQUAL "")
      set(_out_dir "${TARGET_BINARY_DIR}/Test/testsuite")
    else()
      set(_out_dir "${TARGET_BINARY_DIR}/Test/testsuite/${OUT_DIR_NAME}")
    endif()
    get_filename_component(_stem "${WAST_FILE}" NAME_WE)
    set(_json "${_out_dir}/${_stem}.json")
    add_custom_command(
      OUTPUT "${_json}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_out_dir}"
      COMMAND "${WAST2JSON_EXECUTABLE}" ${ARGN} "${WAST_FILE}" -o "${_json}"
      DEPENDS "${WAST_FILE}"
      COMMENT "wast2json: ${_stem}.wast -> ${_stem}.json + .wasm"
      VERBATIM
    )
    add_custom_target(${TARGET_NAME} ALL DEPENDS "${_json}")
    add_dependencies(testsuite_runner ${TARGET_NAME})
  endfunction()

  # ----- KinoRuntime 独自テスト ----------------------------------------------

  # wasi_test: WASI snapshot_preview1 実装の回帰テスト (空 argv/environ)
  _kw_add_wast2json(wasi_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/wasi/wasi_test.wast"
    "")

  # wasi_args_test: 既知の argv/environ をセットして実体検証
  _kw_add_wast2json(wasi_args_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/wasi/wasi_args_test.wast"
    "wasi_args_test")

  # wasi_fs_test: read-only filesystem アクセスの検証
  _kw_add_wast2json(wasi_fs_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/wasi/wasi_fs_test.wast"
    "wasi_fs_test")

  # テスト用データディレクトリを Bin/Test/wasi_fs_data/ に複製
  add_custom_command(
    TARGET testsuite_runner POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/wasi/wasi_fs_data"
      "${TARGET_BINARY_DIR}/Test/wasi_fs_data"
    COMMENT "Copy wasi_fs_data -> Bin/Test/wasi_fs_data"
    VERBATIM
  )

  # wasi_vfs_test: in-memory モック VFS で wasi_set_vfs() の差し替えを検証
  _kw_add_wast2json(wasi_vfs_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/wasi/wasi_vfs_test.wast"
    "wasi_vfs_test")

  # wasi_fs_write_test: 書き込み系 WASI 関数の検証
  _kw_add_wast2json(wasi_fs_write_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/wasi/wasi_fs_write_test.wast"
    "wasi_fs_write_test")

  # 書き込みテスト用の scratch directory を Bin/Test/wasi_fs_write_scratch に作成
  add_custom_command(
    TARGET testsuite_runner POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${TARGET_BINARY_DIR}/Test/wasi_fs_write_scratch"
    COMMENT "Create scratch dir Bin/Test/wasi_fs_write_scratch"
    VERBATIM
  )

  # wasi_fs_advanced_test: Phase 3c (fd_readdir / path_link / path_symlink /
  # path_readlink + path_open(O_DIRECTORY)) の検証
  _kw_add_wast2json(wasi_fs_advanced_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/wasi/wasi_fs_advanced_test.wast"
    "wasi_fs_advanced_test")

  # advanced テスト用 scratch directory
  add_custom_command(
    TARGET testsuite_runner POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${TARGET_BINARY_DIR}/Test/wasi_fs_advanced_scratch"
    COMMENT "Create scratch dir Bin/Test/wasi_fs_advanced_scratch"
    VERBATIM
  )

  # core_shared_table_grow_test: cross-module 共有テーブル table.grow のポインタ/サイズ同期回帰。
  # 2 モジュールで funcref テーブルを共有し importer が grow すると、owner の data ポインタ/size を
  # 同期しないと UAF / 二重 free / stale size になる (kw_core_sync_shared_table_grow が修正)。
  # owner が grow 後サイズを観測でき、realloc 後も pre-grow 要素を呼べることを検証する。
  _kw_add_wast2json(core_shared_table_grow_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/wasi/core_shared_table_grow_test.wast"
    "")

  # core_loop_carry_test: loop への arity-1 / 多値の分岐値運搬回帰 (2026-07-08)。
  # loop 本体は param を home slot から読むが、旧 emit_branch/br_table は arity-1 の
  # back-edge で値を r0 に置くだけだったため param が stale になった (br/br_if/br_table)。
  # 多値 br_if の分岐前 carry が非分岐側の残留 operand を壊すケースも含む。
  _kw_add_wast2json(core_loop_carry_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/wasi/core_loop_carry_test.wast"
    "")

  # core_eh_branch_test: legacy try への arity-1 br (end は home slot 受け) と、
  # catch → block label の値運搬規約 (H_catch は home slot + r0 の両対応) の回帰。
  _kw_add_wast2json(core_eh_branch_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/wasi/core_eh_branch_test.wast"
    ""
    --enable-exceptions)

  # core_shared_table_gfi_test: cross-module 共有テーブルの funcref 表現回帰 (2026-07-08)。
  # 実行時 table.set/fill 等が gfi のまま書くと call_indirect が funcaddr と誤解釈して
  # 別モジュールの関数を呼ぶ。get→set ラウンドトリップ / foreign ref (-2-fa エンコード) /
  # 非共有テーブル経由の呼び出しも検証する。
  _kw_add_wast2json(core_shared_table_gfi_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/wasi/core_shared_table_gfi_test.wast"
    "")

  # wasi_nomem_test: memory 無しモジュールからの WASI 呼び出し回帰 (2026-07-08)。
  # 旧実装は kinowasm_read/write_memory が NULL store を deref してホストごと AV した。
  # 修正後は転送拒否で fd_write が EFAULT (21) を返す。
  _kw_add_wast2json(wasi_nomem_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/wasi/wasi_nomem_test.wast"
    "")

  # core_trytable_carry_test: try_table への arity-1 分岐値運搬 + throw_ref 下の live r0 +
  # try 内 call_indirect の foreign funcref (-2-fa) 復号の回帰 (2026-07-10)。
  # try_table の end は home slot 受けだが旧 emit_branch は r0 渡し (home_tgt が legacy try
  # のみ) で値が消失した。H_call_indirect_eh は foreign 復号を持たず誤 trap した。
  _kw_add_wast2json(core_trytable_carry_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/wasi/core_trytable_carry_test.wast"
    ""
    --enable-exceptions)

  # core_value_carry_test: else 無し if (param 付き) への br と、ブロック境界を跨ぐ
  # live L_REG (r0) の孤立の回帰 (2026-07-10)。前者は param 付き if の home slot 規約統一、
  # 後者は enter_block の入場時 spill で修正。
  _kw_add_wast2json(core_value_carry_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/wasi/core_value_carry_test.wast"
    "")

  # core_eh_invalid_test: catch/catch_all/delegate 境界の結果型検証の回帰 (2026-07-10)。
  # 検証が DROP_STACK_TO_POLYMORPHIC の後にあり常に緩和され、型不足 body が compile へ
  # 渡って AV した。drop 前検証 (kw_parser_validate_try_section_end) で invalid として弾く。
  _kw_add_wast2json(core_eh_invalid_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/wasi/core_eh_invalid_test.wast"
    ""
    --enable-exceptions)

  # core_mem64_test: memory64/table64 のアドレス幅回帰 (2026-07-10)。旧実装は全アドレスを
  # u32 切詰めしていたため 2^32 以上のアドレス/インデックスが折り返して誤アクセスし、
  # grow の 2^32 delta が 0 (成功) と解釈された。
  _kw_add_wast2json(core_mem64_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/wasi/core_mem64_test.wast"
    "")

  # core_else_invalid_test: else 配置検証の回帰 (2026-07-21)。else は if の中でのみ有効だが、
  # block/loop/try 内の else や if の else アーム内の 2 個目の else を parser が通し、core が
  # if 専用 else_fixup を使って bytecode を破壊し AV した。head_opcode で弾く。テキスト形式では
  # 表現できない (wabt が裸の else を拒否) ため (module binary ...) で生バイトを与える。
  _kw_add_wast2json(core_else_invalid_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/wasi/core_else_invalid_test.wast"
    "")

  # core_eh_caught_test: g_caught 台帳 (legacy rethrow) の回帰 (2026-07-10)。handler からの
  # 非局所脱出 (br/br_if/return/throw/delegate) で caught_pop がスキップされ、以後の rethrow N
  # が別の例外を再送出した。try_table catch の台帳汚染 (no-track op 分離) も検証する。
  _kw_add_wast2json(core_eh_caught_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/wasi/core_eh_caught_test.wast"
    ""
    --enable-exceptions)

  # core_refnull_heaptype_test: ref.null heaptype の読み取り幅回帰 (2026-07-10)。
  # parser は heaptype を 1 byte (read_u8) で読むが core は SLEB (rds64) で読むため、
  # 継続ビット付き heaptype で parser/core の命令ストリームが desync した。
  # funcref/externref/exnref のみ受理し、それ以外を malformed として弾く。
  _kw_add_wast2json(core_refnull_heaptype_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/wasi/core_refnull_heaptype_test.wast"
    "")

  # core_import_indextype_test: import matching の index type 回帰 (2026-07-10)。
  # match_limits が is_64/shared を照合せず、memory64/table64 import を memory32/table32
  # インスタンスが充足できた (アドレス型の食い違い)。is_64/shared の完全一致を要求する。
  _kw_add_wast2json(core_import_indextype_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/wasi/core_import_indextype_test.wast"
    "")

  # wasi_efault_test: WASI out ポインタ書込失敗時の EFAULT 返却回帰 (2026-07-10)。
  # 26 箇所の write_memory が戻り値を無視し、範囲外 out ポインタでも ESUCCESS を返して
  # ゲストの結果を取りこぼした。EFAULT (21) 返却に統一 (fd_readdir/path_readlink の
  # 過大報告と random_get の未初期化バッファ乱数化も含む)。
  _kw_add_wast2json(wasi_efault_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/wasi/wasi_efault_test.wast"
    "")

  # core_valtype_invalid_test: valtype バイト検証の回帰 (2026-07-22)。
  # functype (param/result)/globaltype/local 宣言/select_t の型バイトが未検証で、
  # 0x00 (type_stack の任意適合 marker) 等の不正バイトを型として受理し、以後の
  # 型検査を全面すり抜けた。kw_parser_is_valtype の受理リストで弾く。
  _kw_add_wast2json(core_valtype_invalid_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/wasi/core_valtype_invalid_test.wast"
    "")

  # core_host_reexport_test: ホスト関数 reexport の回帰 (2026-07-22)。
  # resolve_imports が extra_func_type 台帳を毎ロード memset で全クリアし、ホスト関数を
  # reexport したモジュール経由の import が台帳 NULL 検査で常に拒否された。台帳を
  # store 寿命の累積 (rebuild_extra_func_type と同セマンティクス) に修正。
  _kw_add_wast2json(core_host_reexport_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/wasi/core_host_reexport_test.wast"
    "")

  # ----- WebAssembly 公式 spec testsuite -------------------------------------
  # ${CMAKE_SOURCE_DIR}/Materials/wasm-spec に github.com/WebAssembly/testsuite
  # をクローンし、wasm-3.0 提案 (typed-references / GC / multi-memory 等) が
  # core wast に流入する直前の commit (2025-09-10 a810159) に固定する。
  # その配下の *.wast を wast2json で展開して
  # ${TARGET_BINARY_DIR}/Test/testsuite/ 直下に配置する。
  # Materials/ は .gitignore 対象なので、リポジトリ自体には含めない。
  set(_WASM_SPEC_DIR "${CMAKE_SOURCE_DIR}/Materials/wasm-spec")
  set(_WASM_SPEC_URL "https://github.com/WebAssembly/testsuite.git")
  # KinoRuntime が想定する WASM 2.0 + reference-types + bulk-memory 範囲。
  # commit を上げる際は wast2json が --enable-* なしで通り、KinoRuntime
  # の既存テストが pass することを確認すること。
  set(_WASM_SPEC_COMMIT "a810159")

  if(NOT EXISTS "${_WASM_SPEC_DIR}/.git")
    find_package(Git QUIET)
    if(GIT_EXECUTABLE)
      message(STATUS "Cloning ${_WASM_SPEC_URL} -> ${_WASM_SPEC_DIR}")
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" clone "${_WASM_SPEC_URL}" "${_WASM_SPEC_DIR}"
        RESULT_VARIABLE _clone_result
      )
      if(NOT _clone_result EQUAL 0)
        message(WARNING "Failed to clone WebAssembly/testsuite (result=${_clone_result})")
      else()
        message(STATUS "Pinning WebAssembly/testsuite to ${_WASM_SPEC_COMMIT}")
        execute_process(
          COMMAND "${GIT_EXECUTABLE}" -C "${_WASM_SPEC_DIR}" checkout --detach "${_WASM_SPEC_COMMIT}"
          RESULT_VARIABLE _checkout_result
        )
        if(NOT _checkout_result EQUAL 0)
          message(WARNING "Failed to checkout ${_WASM_SPEC_COMMIT} (result=${_checkout_result})")
        endif()
      endif()
    else()
      message(WARNING "git not found; cannot auto-clone WebAssembly/testsuite. "
        "Manually clone ${_WASM_SPEC_URL} into ${_WASM_SPEC_DIR} and "
        "checkout commit ${_WASM_SPEC_COMMIT}.")
    endif()
  endif()

  # KinoRuntime 未対応の機能を含む wast はスキップする。
  # - simd_* : SIMD 提案。Phase A1 では simd_const のみ enable。
  # 必要に応じて要素を追加。各エントリは file 名 (NAME_WE 形式) の正規表現。
  set(_WASM_SPEC_EXCLUDE_REGEX "^simd_")
  # 上記 regex で除外されるが、ここに列挙したものは個別に enable する。
  set(_WASM_SPEC_INCLUDE_LIST
    "simd_const" "simd_lane" "simd_bitwise" "simd_boolean"
    "simd_i8x16_arith" "simd_i16x8_arith" "simd_i32x4_arith" "simd_i64x2_arith"
    "simd_i8x16_arith2" "simd_i16x8_arith2" "simd_i32x4_arith2" "simd_i64x2_arith2"
    "simd_i8x16_cmp" "simd_i16x8_cmp" "simd_i32x4_cmp" "simd_i64x2_cmp"
    "simd_f32x4_cmp" "simd_f64x2_cmp"
    "simd_i8x16_sat_arith" "simd_i16x8_sat_arith"
    "simd_f32x4_arith" "simd_f64x2_arith"
    "simd_f32x4_pmin_pmax" "simd_f64x2_pmin_pmax"
    "simd_f32x4" "simd_f64x2"
    "simd_select"
    "simd_bit_shift"
    "simd_int_to_int_extend"
    "simd_conversions"
    "simd_f32x4_rounding" "simd_f64x2_rounding"
    "simd_i32x4_trunc_sat_f32x4" "simd_i32x4_trunc_sat_f64x2"
    "simd_i16x8_q15mulr_sat_s"
    "simd_i16x8_extadd_pairwise_i8x16" "simd_i32x4_extadd_pairwise_i16x8"
    "simd_i16x8_extmul_i8x16" "simd_i32x4_extmul_i16x8" "simd_i64x2_extmul_i32x4"
    "simd_i32x4_dot_i16x8"
    "simd_load" "simd_load_extend" "simd_load_splat" "simd_load_zero"
    "simd_load8_lane" "simd_load16_lane" "simd_load32_lane" "simd_load64_lane"
    "simd_store" "simd_store8_lane" "simd_store16_lane" "simd_store32_lane" "simd_store64_lane"
    "simd_address" "simd_align"
    "simd_splat"
    "simd_linking")

  # spec testsuite の *.wast を一括変換 (configure 時に file(GLOB) で列挙)
  if(EXISTS "${_WASM_SPEC_DIR}")
    file(GLOB _SPEC_WAST_FILES "${_WASM_SPEC_DIR}/*.wast")
    set(_SPEC_JSON_FILES "")
    foreach(_wast IN LISTS _SPEC_WAST_FILES)
      get_filename_component(_stem "${_wast}" NAME_WE)
      if(_stem MATCHES "${_WASM_SPEC_EXCLUDE_REGEX}")
        list(FIND _WASM_SPEC_INCLUDE_LIST "${_stem}" _kept)
        if(_kept EQUAL -1)
          continue()
        endif()
      endif()
      set(_json "${TARGET_BINARY_DIR}/Test/testsuite/${_stem}.json")
      # --enable-all は wast2json の機能フラグ群を全て有効化するが、最近の wabt
      # では --enable-function-references (typed function references) と --enable-gc
      # まで含まれてしまい、commit a810159 の testsuite が想定する legacy semantics
      # (ref.func -> funcref) と齟齬を起こす (例: ref_func.wast / table_grow.wast の
      # global.set / table.set / table.grow / local.set 周辺で type mismatch)。
      # そのため必要な拡張のみを個別に有効化する:
      #   - default ON   : bulk-memory, reference-types, multi-value, simd,
      #                    sign-extension, saturating-float-to-int, mutable-globals
      #   - 明示的に ON  : tail-call, threads, extended-const
      #   - 意図的に OFF : function-references, gc, memory64, multi-memory,
      #                    relaxed-simd, custom-page-sizes, exceptions
      set(_WAST2JSON_FEATURE_FLAGS
        --enable-tail-call
        --enable-threads
        --enable-extended-const)
      add_custom_command(
        OUTPUT "${_json}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${TARGET_BINARY_DIR}/Test/testsuite"
        COMMAND "${WAST2JSON_EXECUTABLE}" ${_WAST2JSON_FEATURE_FLAGS} "${_wast}" -o "${_json}"
        DEPENDS "${_wast}"
        COMMENT "wast2json (spec): ${_stem}.wast"
        VERBATIM
      )
      list(APPEND _SPEC_JSON_FILES "${_json}")
    endforeach()
    if(_SPEC_JSON_FILES)
      add_custom_target(wasm_spec_tests ALL DEPENDS ${_SPEC_JSON_FILES})
      add_dependencies(testsuite_runner wasm_spec_tests)
    endif()
  endif()

  # ----- WASM 3.0 proposal testsuite (allowlist 方式) -------------------
  # 公式 testsuite の proposals 配下から、KinoRuntime が対応している機能の
  # wast ファイルだけを取り込む。allowlist は (proposal_dir, stem) 形式で、
  # 配置先は ${TARGET_BINARY_DIR}/Test/testsuite/proposals/<proposal_dir>/。
  #
  # wast2json のパースは --enable-all で行う (WASM 3.0 構文を網羅するため)。
  # ただし wabt 1.0.x のパーサが扱えない新構文 (definition / 0x1_0000_0000 等)
  # を含むファイルは ここに含めても build エラーになるため除外している。
  #
  # 各 entry: "proposal_subdir|wast_stem"
  set(_WASM3_TESTS
    # === 全 pass する (or 個別 line skip で全 pass) testsuite ===
    # Multi-memory
    "wasm-3.0|memory-multi"
    "wasm-3.0|simd_memory-multi"
    # Memory64 サブセット (table64 を含まないもの)
    "wasm-3.0|address64"
    "wasm-3.0|align64"
    "wasm-3.0|bulk64"
    "wasm-3.0|endianness64"
    "wasm-3.0|float_memory64"
    "wasm-3.0|load64"
    "wasm-3.0|memory_copy64"
    "wasm-3.0|memory_fill64"
    "wasm-3.0|memory_grow64"
    "wasm-3.0|memory_init64"
    "wasm-3.0|memory_redundancy64"
    "wasm-3.0|memory_trap64"
    # WASM 3.0 EH 新仕様 (throw 単体は通る; tag/try_table/throw_ref は wabt
    # parse 不可 / exnref 未対応のため除外)
    "wasm-3.0|throw"
    # WASM 3.0 EH throw_ref (exnref 型対応済み、try_table 経由で動作)
    "wasm-3.0|throw_ref"
    # WASM 3.0 tail call (return_call / return_call_indirect)
    "wasm-3.0|return_call"
    "wasm-3.0|return_call_indirect"
    # WASM 3.0 table64 (i64 indexed tables)
    "wasm-3.0|table_get64"
    "wasm-3.0|table_set64"
    # table_grow64 は spec に独立ファイル無し、table_size64 で兼用
    "wasm-3.0|table_size64"
    "wasm-3.0|table_fill64"
    "wasm-3.0|table_copy64"
    "wasm-3.0|table_init64"
    "wasm-3.0|call_indirect64"
    # Relaxed SIMD: implementation-defined semantics の違いで一部 assert_return が
    # 失敗するが、ほとんど通るので line skip で取り込む
    "wasm-3.0|i32x4_relaxed_trunc"
    "wasm-3.0|relaxed_dot_product"
    "wasm-3.0|relaxed_laneselect"
    "wasm-3.0|relaxed_madd_nmadd"
    "wasm-3.0|relaxed_min_max"
    "wasm-3.0|i16x8_relaxed_q15mulr_s"
    "wasm-3.0|i8x16_relaxed_swizzle"
    # Threads (atomic + supporting wast)
    "threads|atomic"
    "threads|exports"
    "threads|imports"
    "threads|memory"
    # 検証拡充: WASM 3.0 標準系で wabt が parse できる範囲のもの
    "wasm-3.0|bulk"
    "wasm-3.0|memory_copy"
    "wasm-3.0|memory_copy0"
    "wasm-3.0|memory_copy1"
    "wasm-3.0|memory_fill"
    "wasm-3.0|memory_fill0"
    "wasm-3.0|memory_init"
    "wasm-3.0|memory_init0"
    "wasm-3.0|memory_grow"
    "wasm-3.0|memory_size_import"
    "wasm-3.0|memory_size0"
    "wasm-3.0|memory_size1"
    "wasm-3.0|memory_size2"
    "wasm-3.0|memory_size3"
    "wasm-3.0|memory_trap0"
    "wasm-3.0|memory_trap1"
    "wasm-3.0|store0"
    "wasm-3.0|store1"
    "wasm-3.0|store2"
    "wasm-3.0|traps0"
    "wasm-3.0|start0"
    "wasm-3.0|imports"
    "wasm-3.0|exports"
    # 基本制御 / ロード / データセクション (3.0 で multi-memory / memory64
    # 影響があるが wabt 1.0.x で parse できるもの)
    "wasm-3.0|address"
    "wasm-3.0|address0"
    "wasm-3.0|address1"
    "wasm-3.0|align0"
    "wasm-3.0|binary0"
    "wasm-3.0|binary-leb128"
    "wasm-3.0|data0"
    "wasm-3.0|data1"
    "wasm-3.0|data_drop0"
    "wasm-3.0|exports0"
    "wasm-3.0|float_exprs0"
    "wasm-3.0|float_exprs1"
    "wasm-3.0|imports0"
    "wasm-3.0|imports1"
    "wasm-3.0|imports2"
    "wasm-3.0|imports3"
    "wasm-3.0|imports4"
    "wasm-3.0|linking0"
    "wasm-3.0|linking1"
    "wasm-3.0|linking2"
    "wasm-3.0|linking3"
    "wasm-3.0|load0"
    "wasm-3.0|load1"
    "wasm-3.0|load2"
    "wasm-3.0|local_get"
    "wasm-3.0|table_copy_mixed"
    "wasm-3.0|simd_address"
    "wasm-3.0|simd_lane"

    # === 除外している wast (対応必要 or wabt 制約) ======================
    # GC proposal (struct/array/i31/ref_cast 等): GC マネージャ未実装
    #     - array* / struct / i31 / ref_cast / ref_eq / ref_test / ref_*
    #     - br_on_cast* / br_on_non_null / br_on_null / call_ref / return_call_ref
    #     - type-canon / type-equivalence / type-rec / type-subtyping
    # Typed function references (wabt 1.0.x parse 不可): null / func / extern
    # トークン使用のため
    #     - select / linking / try_table / tag / local_init
    # 注釈系 (annotations / id / token): wast 注釈構文専用、runtime 機能なし
    # align64.108.wasm: u64 LEB 10-byte 限界エンコード validation 未対応
    # =====================================================================
  )
  set(_WAST2JSON_W3_FLAGS --enable-all)
  set(_WASM3_JSON_FILES "")
  foreach(_entry IN LISTS _WASM3_TESTS)
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _proposal)
    list(GET _parts 1 _stem)
    set(_wast "${_WASM_SPEC_DIR}/proposals/${_proposal}/${_stem}.wast")
    if(NOT EXISTS "${_wast}")
      continue()
    endif()
    set(_out_dir "${TARGET_BINARY_DIR}/Test/testsuite/proposals/${_proposal}")
    set(_json "${_out_dir}/${_stem}.json")
    add_custom_command(
      OUTPUT "${_json}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_out_dir}"
      COMMAND "${WAST2JSON_EXECUTABLE}" ${_WAST2JSON_W3_FLAGS} "${_wast}" -o "${_json}"
      DEPENDS "${_wast}"
      COMMENT "wast2json (proposal): ${_proposal}/${_stem}.wast"
      VERBATIM
    )
    list(APPEND _WASM3_JSON_FILES "${_json}")
  endforeach()
  if(_WASM3_JSON_FILES)
    add_custom_target(wasm3_proposal_tests ALL DEPENDS ${_WASM3_JSON_FILES})
    add_dependencies(testsuite_runner wasm3_proposal_tests)
  endif()

  # Wide arithmetic proposal: wabt 1.0.x がテキスト形式の i64.add128 等を
  # parse できないため、Python スクリプトで .wasm + .json を生成する。
  # binary module 形式 + assert_return のみ抽出し、assert_invalid は除外。
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    set(_WIDE_WAST "${_WASM_SPEC_DIR}/proposals/wide-arithmetic/wide-arithmetic.wast")
    set(_WIDE_OUT_DIR "${TARGET_BINARY_DIR}/Test/testsuite/proposals/wide-arithmetic")
    set(_WIDE_WASM "${_WIDE_OUT_DIR}/wide-arithmetic.wasm")
    set(_WIDE_JSON "${_WIDE_OUT_DIR}/wide-arithmetic.json")
    set(_WIDE_GEN  "${CMAKE_SOURCE_DIR}/tools/gen_wide_arithmetic.py")
    if(EXISTS "${_WIDE_WAST}" AND EXISTS "${_WIDE_GEN}")
      add_custom_command(
        OUTPUT "${_WIDE_WASM}" "${_WIDE_JSON}"
        COMMAND "${Python3_EXECUTABLE}" "${_WIDE_GEN}" "${_WIDE_WAST}" "${_WIDE_OUT_DIR}"
        DEPENDS "${_WIDE_WAST}" "${_WIDE_GEN}"
        COMMENT "gen wide-arithmetic test artifacts"
        VERBATIM
      )
      add_custom_target(wide_arithmetic_tests ALL DEPENDS "${_WIDE_WASM}" "${_WIDE_JSON}")
      add_dependencies(testsuite_runner wide_arithmetic_tests)
    endif()
  else()
    message(STATUS "Python3 not found; wide-arithmetic tests will be skipped.")
  endif()

  # WASM 3.0 legacy try-catch 系 (proposals/wasm-3.0/legacy)
  set(_WASM3_LEGACY_DIR "${_WASM_SPEC_DIR}/proposals/wasm-3.0/legacy")
  if(EXISTS "${_WASM3_LEGACY_DIR}")
    file(GLOB _WASM3_LEG_WAST_FILES "${_WASM3_LEGACY_DIR}/*.wast")
    set(_WASM3_LEG_JSON_FILES "")
    foreach(_wast IN LISTS _WASM3_LEG_WAST_FILES)
      get_filename_component(_stem "${_wast}" NAME_WE)
      set(_out_dir "${TARGET_BINARY_DIR}/Test/testsuite/proposals/wasm-3.0/legacy")
      set(_json "${_out_dir}/${_stem}.json")
      add_custom_command(
        OUTPUT "${_json}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_out_dir}"
        COMMAND "${WAST2JSON_EXECUTABLE}" ${_WAST2JSON_W3_FLAGS} "${_wast}" -o "${_json}"
        DEPENDS "${_wast}"
        COMMENT "wast2json (wasm-3.0/legacy): ${_stem}.wast"
        VERBATIM
      )
      list(APPEND _WASM3_LEG_JSON_FILES "${_json}")
    endforeach()
    if(_WASM3_LEG_JSON_FILES)
      add_custom_target(wasm3_legacy_proposal_tests ALL DEPENDS ${_WASM3_LEG_JSON_FILES})
      add_dependencies(testsuite_runner wasm3_legacy_proposal_tests)
    endif()
  endif()
else()
  message(WARNING "wast2json not found. Tests won't be (re)built. "
    "Install wabt and re-run cmake, or set WAST2JSON_EXECUTABLE.")
endif()

# ----- WASI testsuite (https://github.com/WebAssembly/wasi-testsuite) -------
# 公式 WASI 互換性テストスイート。pre-built .wasm + .json 構成のため、
# wast2json 不要。Materials/wasi-testsuite/ に auto-clone する (gitignore 配下)。
# Python ランナー (test-runner/wasi_test_runner.py) と adapter
# (tools/wasi-testsuite-adapter/kinoruntime.py) を組み合わせて実行する。
set(_WASI_TS_DIR "${CMAKE_SOURCE_DIR}/Materials/wasi-testsuite")
set(_WASI_TS_URL "https://github.com/WebAssembly/wasi-testsuite.git")
# main ブランチ (時間で変わる) を使うと再現性が落ちるので、安定版にピン留め。
# 取り込み時点 (2026-05-09) の HEAD 付近の v1 ブランチを採用。
set(_WASI_TS_BRANCH "prod/testsuite-base")

if(NOT EXISTS "${_WASI_TS_DIR}/.git")
  find_package(Git QUIET)
  if(GIT_EXECUTABLE)
    message(STATUS "Cloning ${_WASI_TS_URL} -> ${_WASI_TS_DIR}")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" clone --branch "${_WASI_TS_BRANCH}"
              "${_WASI_TS_URL}" "${_WASI_TS_DIR}"
      RESULT_VARIABLE _wasi_clone_result
    )
    if(NOT _wasi_clone_result EQUAL 0)
      message(WARNING "Failed to clone wasi-testsuite (result=${_wasi_clone_result})")
    endif()
  else()
    message(WARNING "git not found; cannot auto-clone wasi-testsuite. "
      "Manually clone ${_WASI_TS_URL} (branch ${_WASI_TS_BRANCH}) into ${_WASI_TS_DIR}.")
  endif()
endif()

if(EXISTS "${_WASI_TS_DIR}")
  # adapter / runner から参照しやすいよう、絶対パスを cache にエクスポート。
  set(KINO_WASI_TESTSUITE_DIR "${_WASI_TS_DIR}" CACHE PATH
    "Path to checked-out wasi-testsuite repository")
endif()

# ----- core (register-TOS) 実行エンジンの単体ドライバ (Test/core/) -----------
# kw_core_run / kw_core_yieldtest / kw_core_apitest / kw_core_multitest。
# core エンジンは保証付き tail-call (musttail) が必須なので、対応コンパイラのときだけ
# ビルドする。KINOWASM_CORE_MUSTTAIL_OK は KinoWASM/CMakeLists.txt が親スコープへ
# エクスポートする (add_subdirectory(KinoWASM) は add_subdirectory(Test) より先)。
if(KINOWASM_CORE_MUSTTAIL_OK)
  add_subdirectory(core)
endif()

# ----- KinoUtil マイクロテスト (Test/util/) ----------------------------------
# kalloc / karray / kbuffer の単体テスト + ベンチ (旧 main.c の ARRAYTEST/COPY_TEST)。
# KinoUtil のみリンクし core エンジン不要なので musttail 可否に依らずビルドする。
add_subdirectory(util)
