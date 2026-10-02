#pragma once

#include <stdint.h>

#if defined(_WIN32)
#if defined(KINO_EXAMPLE_BUILD_DLL)
#define KINO_EXAMPLE_API __declspec(dllexport)
#else
#define KINO_EXAMPLE_API __declspec(dllimport)
#endif
#define KINO_EXAMPLE_CALL __cdecl
#else
#define KINO_EXAMPLE_API
#define KINO_EXAMPLE_CALL
#endif

#if defined(__cplusplus)
extern "C" {
#endif

/* Example-specific ABI, not the public KinoWASM API. One session per DLL.
 * All calls must be serialized on the same thread. open copies the bytes.
 * open: 0=ready, -1=error. tick: 1=suspended, 0=completed, -1=error.
 * A new tick represents a later frame. close also cancels a suspended run. */
KINO_EXAMPLE_API int32_t KINO_EXAMPLE_CALL kino_example_open(const uint8_t* wasm_data, uint32_t wasm_size);
KINO_EXAMPLE_API int32_t KINO_EXAMPLE_CALL kino_example_tick(void);
KINO_EXAMPLE_API int32_t KINO_EXAMPLE_CALL kino_example_progress(void);
KINO_EXAMPLE_API int32_t KINO_EXAMPLE_CALL kino_example_result(void);
KINO_EXAMPLE_API uint32_t KINO_EXAMPLE_CALL kino_example_error(void);
KINO_EXAMPLE_API void KINO_EXAMPLE_CALL kino_example_close(void);

#if defined(__cplusplus)
}
#endif
