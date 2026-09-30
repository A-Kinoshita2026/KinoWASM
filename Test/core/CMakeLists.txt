# ===========================================================================
#  Test/core/ - core (register-TOS) engine standalone driver executables.
#
#  These link KinoWASM.lib only (NOT extrafunction.c / winapi.c / debug.c):
#  the minimal WASI fallback in KinoWASM/core/kw_core_wasi.c provides the
#  clock_time_get / fd_write / args_* host functions the drivers need.
#
#  Built only when the core engine is buildable (guaranteed tail calls), which
#  the parent Test/CMakeLists.txt checks via KINOWASM_CORE_MUSTTAIL_OK before
#  add_subdirectory(core).
#
#    kw_core_run        kw_core_main.c       run a wasm file ([--invoke NAME])
#    kw_core_yieldtest  kw_core_yieldtest.c  host-yield suspend/resume check
#    kw_core_apitest    kw_core_apitest.c    public-API invoke (WASM FUNC ARG..)
#    kw_core_multitest  kw_core_multitest.c  multi-module isolation check
#    kw_core_lifetest   kw_core_lifetest.c   store lifecycle regression check
# ===========================================================================

# target name -> source file
set(_KW_CORE_DRIVERS
	"kw_core_run|kw_core_main.c"
	"kw_core_yieldtest|kw_core_yieldtest.c"
	"kw_core_apitest|kw_core_apitest.c"
	"kw_core_multitest|kw_core_multitest.c"
	"kw_core_lifetest|kw_core_lifetest.c"
	"kw_core_reentrytest|kw_core_reentrytest.c")

foreach(_entry IN LISTS _KW_CORE_DRIVERS)
	string(REPLACE "|" ";" _parts "${_entry}")
	list(GET _parts 0 _tgt)
	list(GET _parts 1 _src)
	add_executable(${_tgt} "${_src}")
	target_include_directories(${_tgt} PRIVATE
		"${CMAKE_SOURCE_DIR}/host"                       # kw_core_mem_backend_win.h
		"${CMAKE_SOURCE_DIR}/KinoWASM"              # kinowasm.h / features.h / core/kw_core.h
		"${CMAKE_SOURCE_DIR}/KinoWASM/KinoUtil"     # kalloc.h / karray.h
		"${CMAKE_SOURCE_DIR}/KinoWASM/core")        # kw_core.h ("kw_core.h")
	target_link_libraries(${_tgt} PRIVATE KinoWASM)
	# Mirror the other targets: copy the exe next to KinoRuntime.exe (Bin/).
	add_custom_command(TARGET ${_tgt} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy
			$<TARGET_FILE_NAME:${_tgt}> "${TARGET_BINARY_DIR}/"
		VERBATIM)
endforeach()

# yield_test.wasm: input for kw_core_yieldtest. Generated from yield_test.wat
# with wat2wasm (wabt) and placed next to the exe (Bin/) so the test finds it
# with a CWD-relative path. wast2json's sibling wat2wasm is used.
find_program(KW_CORE_WAT2WASM wat2wasm HINTS "D:/Tools/wabt/bin"
	DOC "wat2wasm from wabt for building yield_test.wasm")
if(KW_CORE_WAT2WASM)
	set(_YIELD_WAT  "${CMAKE_CURRENT_SOURCE_DIR}/yield_test.wat")
	set(_YIELD_WASM "${TARGET_BINARY_DIR}/yield_test.wasm")
	add_custom_command(
		OUTPUT "${_YIELD_WASM}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${TARGET_BINARY_DIR}"
		COMMAND "${KW_CORE_WAT2WASM}" "${_YIELD_WAT}" -o "${_YIELD_WASM}"
		DEPENDS "${_YIELD_WAT}"
		COMMENT "wat2wasm: yield_test.wat -> yield_test.wasm"
		VERBATIM)
	# eh_yield_test.wasm: part 2 input (exception thrown after a resume).
	set(_EH_YIELD_WAT  "${CMAKE_CURRENT_SOURCE_DIR}/eh_yield_test.wat")
	set(_EH_YIELD_WASM "${TARGET_BINARY_DIR}/eh_yield_test.wasm")
	add_custom_command(
		OUTPUT "${_EH_YIELD_WASM}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${TARGET_BINARY_DIR}"
		COMMAND "${KW_CORE_WAT2WASM}" --enable-exceptions "${_EH_YIELD_WAT}" -o "${_EH_YIELD_WASM}"
		DEPENDS "${_EH_YIELD_WAT}"
		COMMENT "wat2wasm: eh_yield_test.wat -> eh_yield_test.wasm"
		VERBATIM)
	add_custom_target(kw_core_yield_test_wasm ALL DEPENDS "${_YIELD_WASM}" "${_EH_YIELD_WASM}")
	add_dependencies(kw_core_yieldtest kw_core_yield_test_wasm)

	# yieldtest is deterministic and self-contained: register it with CTest.
	add_test(NAME kw_core_yieldtest COMMAND kw_core_yieldtest)
	set_tests_properties(kw_core_yieldtest PROPERTIES
		WORKING_DIRECTORY "${TARGET_BINARY_DIR}"
		PASS_REGULAR_EXPRESSION "PASS: EH across resume OK")

	# life_ok.wasm: input for kw_core_lifetest (store lifecycle regressions).
	# The failed-build case no longer needs a .wasm file: the parser rejects every
	# opcode the core cannot execute, so the test hands a raw image straight to
	# kw_core_build_from_store (see kw_core_lifetest.c).
	set(_LIFE_OK_WAT   "${CMAKE_CURRENT_SOURCE_DIR}/life_ok.wat")
	set(_LIFE_OK_WASM  "${TARGET_BINARY_DIR}/life_ok.wasm")
	add_custom_command(
		OUTPUT "${_LIFE_OK_WASM}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${TARGET_BINARY_DIR}"
		COMMAND "${KW_CORE_WAT2WASM}" "${_LIFE_OK_WAT}" -o "${_LIFE_OK_WASM}"
		DEPENDS "${_LIFE_OK_WAT}"
		COMMENT "wat2wasm: life_ok.wat -> life_ok.wasm"
		VERBATIM)
	add_custom_target(kw_core_life_test_wasm ALL DEPENDS "${_LIFE_OK_WASM}")
	add_dependencies(kw_core_lifetest kw_core_life_test_wasm)

	add_test(NAME kw_core_lifetest COMMAND kw_core_lifetest)
	set_tests_properties(kw_core_lifetest PROPERTIES
		WORKING_DIRECTORY "${TARGET_BINARY_DIR}"
		PASS_REGULAR_EXPRESSION "LIFETEST OK")

	# reentry_{a,b,c}.wasm: inputs for kw_core_reentrytest (host re-entry taken
	# while a cross-module call is on the stack).
	set(_REENTRY_WASMS "")
	foreach(_m a b c)
		set(_REENTRY_WAT  "${CMAKE_CURRENT_SOURCE_DIR}/reentry_${_m}.wat")
		set(_REENTRY_WASM "${TARGET_BINARY_DIR}/reentry_${_m}.wasm")
		add_custom_command(
			OUTPUT "${_REENTRY_WASM}"
			COMMAND ${CMAKE_COMMAND} -E make_directory "${TARGET_BINARY_DIR}"
			COMMAND "${KW_CORE_WAT2WASM}" "${_REENTRY_WAT}" -o "${_REENTRY_WASM}"
			DEPENDS "${_REENTRY_WAT}"
			COMMENT "wat2wasm: reentry_${_m}.wat -> reentry_${_m}.wasm"
			VERBATIM)
		list(APPEND _REENTRY_WASMS "${_REENTRY_WASM}")
	endforeach()
	add_custom_target(kw_core_reentry_test_wasm ALL DEPENDS ${_REENTRY_WASMS})
	add_dependencies(kw_core_reentrytest kw_core_reentry_test_wasm)

	add_test(NAME kw_core_reentrytest COMMAND kw_core_reentrytest)
	set_tests_properties(kw_core_reentrytest PROPERTIES
		WORKING_DIRECTORY "${TARGET_BINARY_DIR}"
		PASS_REGULAR_EXPRESSION "REENTRY OK")
else()
	message(WARNING "wat2wasm not found; kw_core_yieldtest / kw_core_lifetest inputs "
		"will not be (re)built and the CTest entries are skipped.")
endif()
