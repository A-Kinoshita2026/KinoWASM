find_program(KINO_EXAMPLE_WAT2WASM wat2wasm HINTS "D:/Tools/wabt/bin" REQUIRED)
set(_example_wasm "${CMAKE_CURRENT_BINARY_DIR}/cooperative.wasm")
add_custom_command(OUTPUT "${_example_wasm}"
	COMMAND "${KINO_EXAMPLE_WAT2WASM}" "${CMAKE_CURRENT_SOURCE_DIR}/cooperative.wat" -o "${_example_wasm}"
	DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cooperative.wat"
	VERBATIM)
add_custom_target(kino_cooperative_guest ALL DEPENDS "${_example_wasm}")

add_library(kino_cooperative SHARED cooperative_bridge.c "${CMAKE_SOURCE_DIR}/host/systemmemory.c")
target_compile_definitions(kino_cooperative PRIVATE KINO_EXAMPLE_BUILD_DLL)
target_include_directories(kino_cooperative PRIVATE
	"${CMAKE_SOURCE_DIR}/host" "${CMAKE_SOURCE_DIR}/KinoWASM"
	"${CMAKE_SOURCE_DIR}/KinoWASM/KinoUtil")
target_link_libraries(kino_cooperative PRIVATE KinoWASM)
add_executable(kino_cooperative_demo main.c)
target_link_libraries(kino_cooperative_demo PRIVATE kino_cooperative)
add_dependencies(kino_cooperative_demo kino_cooperative_guest)
add_test(NAME kino_cooperative_demo COMMAND kino_cooperative_demo "${_example_wasm}")
set_tests_properties(kino_cooperative_demo PROPERTIES
	PASS_REGULAR_EXPRESSION "reopen and cancellation OK")
