# Embedding KinoWASM

English | [日本語](ja/QuickGuide.md)

This guide covers the application-side lifecycle. See [Public API](Public-API.md) for individual contracts and [Build and test](Build-and-Test.md) for supported toolchains.

## Initialization order

1. Install a linear-memory backend.
2. Select a system allocation arena.
3. Register the host imports your module needs.
4. Create a store and assign its caller-owned memory.
5. Initialize a module arena and load the module.
6. Invoke an export and inspect its result.
7. Release arrays/runtime state, then free backing buffers.

The following Windows example runs an import-free module exporting `_start` with no arguments or results. A WASI module additionally needs the appropriate host functions registered. The buffer sizes are example budgets, not universal minimums.

```c
#include <stdio.h>
#include <stdlib.h>
#include "kinowasm.h"
#include "systemmemory.h"
#include "kw_core_mem_backend_win.h"

/* Minimal environment accessors required by the core WASI fallback.
 * Omit these when linking host/extrafunction.c, which provides them. */
static const char* const empty_argv[] = { NULL };

int wasi_get_argc(void)
{
	return 0;
}

const char* const* wasi_get_argv(void)
{
	return empty_argv;
}

const char* const* wasi_get_envp(void)
{
	return NULL;
}

int main(void)
{
	const size_t store_size = 100 * 1024 * 1024;
	const size_t module_size = 20 * 1024 * 1024;
	void* store_buffer = malloc(store_size);
	void* module_buffer = malloc(module_size);
	kinowasm_handle_t store = NULL;
	kinowasm_args_t args = { 0 };
	kinowasm_result_t result = RES_ERROR;
	if(store_buffer == NULL || module_buffer == NULL)
		goto cleanup;

	kw_core_install_default_mem_backend();
	change_system_memory();
	store = kinowasm_init();
	if(store == NULL)
		goto cleanup;

	kinowasm_assign_memory(store, store_buffer, store_size);
	kinowasm_mem_info_t module_memory = kinowasm_mem_info_init(module_buffer, module_size);
	if(module_memory == NULL)
		goto cleanup;

	result = kinowasm_load_module(store, "example.wasm", "sample", module_memory);
	if(result != RES_SUCCESS)
		goto cleanup;

	result = kinowasm_array_new_from(args, 0);
	if(result == RES_SUCCESS)
		result = kinowasm_invoke(store, "sample", "_start", &args);

cleanup:
	kinowasm_array_term_from(args);
	if(store != NULL)
		kinowasm_term(store);

	free(module_buffer);
	free(store_buffer);
	if(result != RES_SUCCESS)
		fprintf(stderr, "KinoWASM result: %u\n", (unsigned)result);

	return result == RES_SUCCESS ? 0 : 1;
}
```

## Parameters and return values

Create an argument array with the number of parameters expected by the export, set each `type` and value, and check the invocation result before reading the array. Invocation replaces input values with results; refill parameters for the next call. See the example in [Public API](Public-API.md#argument-arrays).

For repeated calls, resolve once with `kinowasm_lookup_func` and use `kinowasm_invoke_func`. Handles become invalid when their module is removed/reset/reloaded.

## Imports and memory

Host registration names must match WASM import module/function names exactly. [Host functions](Host-Functions.md) explains callback arguments, return slots and WASI registration.

WASM pointers are offsets. In callbacks, use `kinowasm_read_memory` and `kinowasm_write_memory`, check the returned result, and never cast an offset to a native pointer. The Windows backend uses OS virtual-memory facilities; implement the backend interface for other platforms rather than assuming the Windows helper is portable.

## Multiple modules and suspension

Load dependencies before modules importing their exports. Use distinct module names and keep the associated arena buffers alive. The reference host layer also provides a push/invoke/pop module lifecycle; it is separate from the basic public API.

When a host operation intentionally suspends execution, preserve store/frame state and resume with `kinowasm_resume` after completing that operation. Do not treat every error code as suspension. See [Host functions](Host-Functions.md#suspension-and-module-lifecycle).

## CMake integration

To incorporate only the runtime subproject, use:

```cmake
add_subdirectory(third_party/KinoRuntime/KinoWASM)
add_executable(my_app main.c
    third_party/KinoRuntime/host/systemmemory.c)
target_link_libraries(my_app PRIVATE KinoWASM)
target_include_directories(my_app PRIVATE
    third_party/KinoRuntime/KinoWASM
    third_party/KinoRuntime/KinoWASM/KinoUtil
    third_party/KinoRuntime/host)
```

The parent project must enable C, provide supported compiler options and satisfy `musttail` requirements. In particular, MSVC needs optimization and settings compatible with guaranteed tail calls, including in Debug; use the top-level build as a reference. The library subproject does not inherit the top-level project's compiler settings when embedded by itself.

If using the shared WASI host implementation, also compile `host/extrafunction.c`, `host/winapi.c`, and `host/debug.c`, and add `KinoWASM/store` to include paths. These sources use internal interfaces; they are reference host code rather than part of the public C API.

When manually linking `.lib` files, locate both `KinoWASM.lib` and `KinoUtil.lib` in the build tree and match their configuration to the application. Do not assume that a library was copied to `Bin/`.

## Common problems

| Problem | Check |
|---|---|
| Initialization/allocation fails | Select a valid system arena first and check native allocations |
| Load runs out of memory | Increase the store/module budgets for the actual module |
| Import is unresolved | Check exact names, registration order and dependency loading |
| Linear-memory backend is missing | Install it before running a module with memory |
| Memory transfer fails | Check offsets, lengths and the transfer result |
| Function handle stops working | Resolve again after reset/reload |

The allocator selection and host registry include global state. Separate stores alone do not guarantee safe parallel execution; synchronize integration state.
