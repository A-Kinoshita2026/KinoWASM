# ===========================================================================
#  Test/util/ - KinoUtil (kalloc / karray / kbuffer) microtests + benchmarks.
#
#  Single driver `kutil_microtest <copytest|arraytest1|arraytest2|arraytest3>`.
#  Formerly the ARRAYTEST1/2/3 and COPY_TEST compile gates inside main.c.
#  Links KinoUtil only (+ winapi.c for the perf timer); no core engine needed,
#  so this builds regardless of musttail support.
# ===========================================================================

add_executable(kutil_microtest
	"kutil_microtest.c"
	"${CMAKE_SOURCE_DIR}/host/winapi.c")          # perf timer (init/begin/end/print_performaince)
target_include_directories(kutil_microtest PRIVATE
	"${CMAKE_SOURCE_DIR}/host"                     # winapi.h
	"${CMAKE_SOURCE_DIR}/KinoWASM/KinoUtil")  # kalloc.h / karray.h / kbuffer.h
target_link_libraries(kutil_microtest PRIVATE KinoUtil)

# Mirror the other targets: copy the exe next to KinoRuntime.exe (Bin/).
add_custom_command(TARGET kutil_microtest POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy
		$<TARGET_FILE_NAME:kutil_microtest> "${TARGET_BINARY_DIR}/"
	VERBATIM)

add_executable(kbuffer_test "kbuffer_test.c")
target_include_directories(kbuffer_test PRIVATE "${CMAKE_SOURCE_DIR}/KinoWASM/KinoUtil")
target_link_libraries(kbuffer_test PRIVATE KinoUtil)
add_test(NAME kbuffer_test COMMAND kbuffer_test)
