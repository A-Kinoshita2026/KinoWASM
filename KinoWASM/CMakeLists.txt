cmake_minimum_required(VERSION 3.18)

project(KinoWASM)

# ===========================================================================
#  KinoWASM ランタイム本体 (静的ライブラリ KinoWASM.lib)。
#  「KinoWASM 側の処理」(feature gate / ソース一覧) はすべて
#  ここに集約する。ルート (KinoRuntime) からは add_subdirectory(KinoWASM) で
#  取り込み、KinoWASM を link する側 (実行ファイル / testsuite_runner / perf) は
#  feature gate を PUBLIC usage requirement として自動継承する。
#  この CMakeLists 単体でもライブラリだけ standalone にビルドできる。
# ===========================================================================

# ----- Build-time feature gates --------------------------------------------
# OFF にすると該当機能を含む wasm モジュールは load 時に ERR_FEATURE_DISABLED で
# reject される。バイナリサイズも縮む。詳細は features.h 参照。
# SIMD は既定 OFF: SIMD を使う wasm を標準生成するツールチェインは稀で、有効時は
# バイナリが ~25-50KB 増えるため。SIMD wasm を扱う場合のみ -DKINOWASM_ENABLE_SIMD=ON。
# 注: core は 0xFD を実装していないため ON はパーサ受理までで、実行はできない。
# threads/atomics (0xFE) は core 未実装のためパーサで常時拒否 (gate なし)。
# exception handling は core が完全実装、WASI / 拡張ホスト関数は core 非関与のため常時有効。
option(KINOWASM_ENABLE_SIMD      "Enable WASM SIMD (0xFD prefix, ~232 op)"            OFF)

set(KINOWASM_SRCS
 "parser/kw_parser.c"
 "store/kw_store.c"
 "store/kw_store_alloc.c"
 "kinowasm.c"
 "cpp.hint"
)

# ----- core エンジンの musttail 要件判定 (ライブラリ自己完結) --------------------
# core (唯一の実行エンジン) は保証付き tail-call (musttail) が必須。clang
# (__attribute__((musttail))) と MSVC 14.50+ ([[msvc::musttail]]、MSVC_VERSION>=1950) が対応。
# 判定をここ (lib 側) に置くことで、KinoWASM を add_subdirectory する別プロジェクトや
# このディレクトリ単体ビルドでも要件が効く。親があるときは親スコープへもエクスポートし、
# ルート / Test の add_subdirectory(core) 条件などから参照できるようにする。
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
	set(KINOWASM_CORE_MUSTTAIL_OK ON)
elseif(CMAKE_C_COMPILER_ID STREQUAL "MSVC" AND MSVC_VERSION GREATER_EQUAL 1950)
	set(KINOWASM_CORE_MUSTTAIL_OK ON)
else()
	set(KINOWASM_CORE_MUSTTAIL_OK OFF)
endif()
# 親スコープへエクスポート (このディレクトリが add_subdirectory で取り込まれている場合のみ。
# CMAKE_SOURCE_DIR 比較は PROJECT_IS_TOP_LEVEL より古い CMake でも安全)。
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	set(KINOWASM_CORE_MUSTTAIL_OK ${KINOWASM_CORE_MUSTTAIL_OK} PARENT_SCOPE)
endif()
if(NOT KINOWASM_CORE_MUSTTAIL_OK)
	# core が唯一の実行エンジン。musttail 非対応コンパイラでは KinoWASM をビルドできない。
	message(FATAL_ERROR "KinoWASM core エンジンには保証付き tail-call (musttail) が必須です "
		"(clang / clang-cl か MSVC 14.50+)。対応コンパイラを使用してください。")
endif()

# ----- core (register-TOS) 実行エンジンの lib 取り込み ----------------
# core は唯一の実行エンジン。musttail 対応コンパイラ (clang / MSVC 14.50+) のとき、core 実行
# エンジン本体 (kw_core_compile/exec/wasi/bridge) を KinoWASM.lib に取り込む。これにより
# testsuite_runner / 実行ファイルは公開 API のまま core エンジンで駆動される。kw_core_main.c は
# 単体ドライバ (kw_core_run) 専用なので lib には含めない (Test/core/ でビルド)。
# KINOWASM_CORE_MUSTTAIL_OK はルート CMakeLists が定義 (非対応コンパイラはルートで FATAL_ERROR)。
set(_kw_core_enabled OFF)
if(KINOWASM_CORE_MUSTTAIL_OK)
	set(_kw_core_enabled ON)
	list(APPEND KINOWASM_SRCS
		"core/kw_core_compile.c"
		"core/kw_core_exec.c"
		"core/kw_core_wasi.c"
		"core/kw_core_bridge.c")
endif()

add_subdirectory(KinoUtil)

add_library(${PROJECT_NAME} STATIC ${KINOWASM_SRCS})
# store/ は KinoWASM 本体から分離したサブフォルダ。store/ 内の kw_store.c は本体ディレクトリの
# module.h 等を、本体 (kinowasm.c) や外部 (extrafunction.c 等) は store/ の kw_store.h を
# "" include で解決できるよう、本体ディレクトリと store/ を通す。
target_include_directories(${PROJECT_NAME} PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_CURRENT_SOURCE_DIR}/store"
	"${CMAKE_CURRENT_SOURCE_DIR}/KinoUtil")
target_link_libraries(${PROJECT_NAME} PRIVATE KinoUtil)

# core ソースは core/ 配下なので kw_core.h / kw_core_ops.h を "" include で解決できるよう
# core/ を include パスに通す (本体・store ディレクトリは上で追加済)。
if(_kw_core_enabled)
	target_include_directories(${PROJECT_NAME} PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/core")
	message(STATUS "KinoWASM: core 実行エンジンを lib に取り込み (id='${CMAKE_C_COMPILER_ID}' ${CMAKE_C_COMPILER})")
endif()

# feature gate define は PUBLIC usage requirement にして、KinoWASM を link する利用側
# (KinoRuntime 実行ファイル / testsuite_runner / perf) にも伝播させる。store の const 式
# 評価 (v128) が lib 側と一致する必要があるため。
if(NOT KINOWASM_ENABLE_SIMD)
	target_compile_definitions(${PROJECT_NAME} PUBLIC KINOWASM_ENABLE_SIMD=0)
endif()
