#cmake_minimum_required(VERSION 2.8...3.13)
cmake_minimum_required(VERSION 4.2)

set(TARGET_BINARY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Bin")

project(KinoRuntime)

# ===========================================================================
#  KinoRuntime: KinoWASM ランタイムを駆動する実行ファイル + 計測ハーネス。
#  KinoWASM ライブラリ本体 (feature gate / execcodegen / ソース) の処理は
#  KinoWASM/CMakeLists.txt に集約し、ここからは add_subdirectory(KinoWASM) で
#  取り込む。下記の「build 全体に効く設定」(adventure / opcode profiler /
#  コンパイラフラグ) は lib (subdir) にも伝播させるため add_subdirectory(KinoWASM)
#  より *前* に置く。
# ===========================================================================

# adventure: 実験的レイアウト摂動機能。adventure.c (実行ファイル側ソース) を追加し、
# lib (runtime_exec.h) と exe (main.c) の双方が -DADVENTURE を見る必要があるので、
# add_subdirectory(KinoWASM) より前で global に定義する。
option(KINOWASM_ENABLE_ADVENTURE "Enable experimental adventure features" ON)
if(KINOWASM_ENABLE_ADVENTURE)
	add_definitions(-DADVENTURE)
endif()

add_definitions(-D_CRT_SECURE_NO_WARNINGS)

# KinoUtil マイクロテスト (旧 ARRAYTEST1/2/3 / COPY_TEST) は Test/util/kutil_microtest へ移設。
#add_definitions(-DKINOWASM_OPCODE_COUNTER)
#add_definitions(-DKINOWASM_ALLOC_DEBUG)
#add_definitions(-DLINEAR_MEMORY_DEBUG)

# 性能計測デバッグモード: opcode 別の実行回数 + 累積実行サイクル (rdtsc) を計測し、
# 終了時に opcode_profile.csv へ書き出す。KINOWASM_OPCODE_COUNTER を内包する。
# 全 TU (runtime.c / debug.c / main.c) で両マクロが見えるよう、ここ (subdir より前) で
# global に定義する。OFF が既定 (Release 性能に影響しない)。
# 有効化: cmake -DKINOWASM_OPCODE_PROFILE=ON
option(KINOWASM_OPCODE_PROFILE "Enable per-opcode count + cycle profiler (debug)" OFF)
if(KINOWASM_OPCODE_PROFILE)
	add_definitions(-DKINOWASM_OPCODE_PROFILE -DKINOWASM_OPCODE_COUNTER)
endif()

# core エンジン動的 opcode プロファイラ: NEXT 毎に op / op-pair の実行回数を数え、
# 終了時に core_prof_dump() が stderr へ降順出力する (kw_core_exec.c)。計測は重いが
# カウントは正確。OFF が既定 (Release 性能に影響しない)。
# 有効化: cmake -DKINOWASM_CORE_PROFILE=ON
option(KINOWASM_CORE_PROFILE "Enable core engine dynamic opcode/pair profiler (debug)" OFF)
if(KINOWASM_CORE_PROFILE)
	add_definitions(-DCORE_PROFILE)
endif()

# コンパイラフラグ (lib + exe 共通)。add_subdirectory(KinoWASM) より前に設定して
# サブディレクトリ (KinoWASM ライブラリ) へ CMAKE_C_FLAGS を伝播させる。
if(CMAKE_BUILD_TYPE STREQUAL Release)
	if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
		# /clang:-falign-functions=32: ホット関数 (core dispatch の H_* musttail 群) を 32B 境界へ
		# 揃え、ホットパス無変更でも無関係なコミットでコード配置がずれて CoreMark が ±8% 振れる
		# 「アライメントのスイートスポット」現象を抑止する (実測: 振れ 7.3%->0.2%, avg +4.2%)。
		# clang-cl は -falign-functions を unknown argument として無視するため /clang: で素通しする。
		# 64B は +15KB のコード膨張で I-cache を圧迫しフロアが下がるため、32B が最適点。
		set(CMAKE_C_FLAGS "/std:c11 /Zi /O2 /GS- -mavx2 -mbmi /clang:-falign-functions=32 -Wno-unused-parameter -Wno-sign-conversion -Wno-implicit-int-conversion -Wno-shorten-64-to-32 -Wno-initializer-overrides")
	else()
		set(CMAKE_C_FLAGS "/std:clatest /Zi /O2 /Ob2 /Oi /GS- /arch:AVX2 /we4245 /we4365 /Wall /wd4711 /wd4820 /wd4668 /wd4710 /wd5045 /wd4100 /wd4505 /WX")
	endif()
	add_link_options(/INCREMENTAL:NO /OPT:REF /OPT:ICF /DEBUG)
	#add_link_options(/INCREMENTAL:NO /OPT:REF /OPT:ICF)
else()
	if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
		set(CMAKE_C_FLAGS "/std:c11 /Zi /GS- -mavx2 -mbmi -Wno-unused-parameter -Wno-sign-conversion -Wno-implicit-int-conversion -Wno-shorten-64-to-32 -Wno-initializer-overrides")
	else()
		set(CMAKE_C_FLAGS "/std:clatest /Zi /O2 /Ob2 /Oi /GS- /arch:AVX2 /we4245 /we4365 /Wall /wd4711 /wd4820 /wd4668 /wd4710 /wd5045 /wd4100 /wd4505 /WX")
	endif()
endif()

# MSVC musttail requires optimization, including Debug. Keep symbols and
# remove runtime checks that conflict with /O2; retain the Debug CRT.
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
	set(CMAKE_MSVC_RUNTIME_CHECKS "")
	string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
	string(REPLACE "/Od" "/O2" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
	string(REPLACE "/Ob0" "/Ob2" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
endif()

# ----- MSVC PGO (Profile-Guided Optimization) ------------------------------
# フロア(非ホットスポット)のアライメント振れを潰す目的。ホット関数を訓練プロファイルで密集配置し、
# レイアウト運に左右されず常にスイートスポット相当へ張り付かせる。2 段 build:
#   1) GEN: /GENPROFILE 付きで計測版を build → CoreMark を数回走らせ .pgc を生成。
#   2) USE: /USEPROFILE で本番最適化 (objects は /GL のまま不変、relink のみ)。
# 既定 OFF・MSVC 限定 (clang は /clang:-falign-functions=32 で対処済)。既存 build に無影響。
option(KINOWASM_PGO_GEN "MSVC PGO instrument build (/GENPROFILE)" OFF)
option(KINOWASM_PGO_USE "MSVC PGO optimized build (/USEPROFILE)" OFF)
if((KINOWASM_PGO_GEN OR KINOWASM_PGO_USE) AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
	# /GL=全プログラム最適化 (LTCG/PGO の前提)。/wd4961=訓練データの無い exe (testsuite_runner 等、
	# CoreMark で訓練するのは KinoRuntime のみ) を /USEPROFILE で link する際の C4961 (プロファイル
	# 未マージ警告) を /WX エラー化させない。未訓練 exe は PGO 無しで普通に build される。
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /GL /wd4961")
	if(KINOWASM_PGO_GEN)
		add_link_options(/LTCG /GENPROFILE /MAP)
	else()
		add_link_options(/LTCG /USEPROFILE)
	endif()
endif()

# ----- KinoWASM ランタイムライブラリ ---------------------------------------
# feature gate / execcodegen / KINOWASM_SRCS はすべて KinoWASM/CMakeLists.txt 側。
# feature gate define は KinoWASM の PUBLIC usage requirement として実行ファイルへ伝播する。
add_subdirectory(KinoWASM)

# ----- KinoRuntime 実行ファイル + 計測ハーネス ------------------------------
set(KINORUNTIME_HEADERS
	"${CMAKE_CURRENT_SOURCE_DIR}/host"
	"${CMAKE_CURRENT_SOURCE_DIR}/KinoWASM/KinoUtil"
	"${CMAKE_CURRENT_SOURCE_DIR}/KinoWASM"
	"${CMAKE_CURRENT_SOURCE_DIR}/KinoWASM/store"
)

set(KINORUNTIME_SRCS
	"host/systemmemory.c"
	"host/winapi.c"
	"host/extrafunction.c"
	"apps/KinoRuntime/main.c"
	"host/debug.c"
)
if(KINOWASM_ENABLE_ADVENTURE)
	list(APPEND KINORUNTIME_SRCS "apps/KinoRuntime/adventure.c")
endif()

set(KINORUNTIME_PERF_SRCS
	"host/systemmemory.c"
	"host/winapi.c"
	"host/extrafunction.c"
	"host/debug.c"
	"Test/perf_invoke_func2.c"
)

add_executable(${PROJECT_NAME} ${KINORUNTIME_SRCS})
add_executable(KinoRuntimePerfInvokeFunc2 ${KINORUNTIME_PERF_SRCS})
# ロード時間計測ハーネス (import を全 stub 化して任意 wasm のロードのみ計時)
add_executable(KinoRuntimePerfLoad "host/systemmemory.c" "Test/perf_load.c")
# 動的モジュール push/pop lifecycle 回帰テスト (rollback_modulepush の解放漏れ検出)
add_executable(KinoRuntimePushPopTest "host/systemmemory.c" "host/winapi.c" "host/extrafunction.c" "host/debug.c" "Test/pushpop_test.c")
target_include_directories(${PROJECT_NAME} PRIVATE ${KINORUNTIME_HEADERS})
target_include_directories(KinoRuntimePerfInvokeFunc2 PRIVATE ${KINORUNTIME_HEADERS} "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(KinoRuntimePerfLoad PRIVATE ${KINORUNTIME_HEADERS} "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(KinoRuntimePushPopTest PRIVATE ${KINORUNTIME_HEADERS} "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(${PROJECT_NAME} PRIVATE KinoWASM)
target_link_libraries(KinoRuntimePerfInvokeFunc2 PRIVATE KinoWASM)
target_link_libraries(KinoRuntimePerfLoad PRIVATE KinoWASM)
target_link_libraries(KinoRuntimePushPopTest PRIVATE KinoWASM)

if(MSVC)
	target_link_options(KinoRuntimePerfInvokeFunc2 PRIVATE
		"/LIBPATH:$ENV{VCToolsInstallDir}lib\\x64"
		"/LIBPATH:$ENV{UniversalCRTSdkDir}Lib\\$ENV{UCRTVersion}\\ucrt\\x64"
		"/LIBPATH:$ENV{WindowsSdkDir}Lib\\$ENV{UCRTVersion}\\um\\x64"
	)
endif()

add_custom_command(
  TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E echo "Copying program."
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE_NAME:${PROJECT_NAME}> "${TARGET_BINARY_DIR}/"
)
# PerfInvokeFunc2 は自身の POST_BUILD で自分をコピーする。PGO の clean ビルドで exe を
# 一旦消す際、KinoRuntime の POST_BUILD が未ビルドの sibling exe をコピーしようとして
# 失敗するのを防ぐ (scripts/pgo_build.bat 参照)。
add_custom_command(
  TARGET KinoRuntimePerfInvokeFunc2 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE_NAME:KinoRuntimePerfInvokeFunc2> "${TARGET_BINARY_DIR}/"
)
add_custom_command(
  TARGET KinoRuntimePerfLoad POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE_NAME:KinoRuntimePerfLoad> "${TARGET_BINARY_DIR}/"
)

set_target_properties(
	${PROJECT_NAME} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${TARGET_BINARY_DIR}/"
)

enable_testing()
add_subdirectory(Test)

option(KINORUNTIME_BUILD_EXAMPLES "Build cooperative main-thread examples" OFF)
if(KINORUNTIME_BUILD_EXAMPLES)
	add_subdirectory(examples/cooperative)
endif()
