# Public C API

English | [日本語](ja/Public-API.md)

The runtime interface is declared in `KinoWASM/kinowasm.h`. Value/argument definitions are in `kargs.h`; allocation and arrays are in `KinoUtil/kalloc.h` and `KinoUtil/karray.h`. Link both the runtime and its KinoUtil dependency when linking static libraries manually. With CMake, use the `KinoWASM` target.

The core WASI fallback also references `wasi_get_argc`, `wasi_get_argv`, and `wasi_get_envp`. Provide those accessors in the host application, or link the shared `host/extrafunction.c` implementation. The embedding example supplies an empty environment.

## Types

| Type | Meaning |
|---|---|
| `kinowasm_handle_t` | Opaque runtime/store handle |
| `kinowasm_mem_info_t` | Allocation arena descriptor backed by caller-provided memory |
| `kinowasm_result_t` | Result code; compare against `RES_SUCCESS` |
| `kinowasm_funcref_t` | Resolved exported function handle; `KINOWASM_FUNCREF_INVALID` is invalid |
| `kinowasm_arg_t` | A value with a type tag and `val` union |
| `kinowasm_args_t` | Dynamic array of typed values; contains `data`, `item_size`, `capacity`, `len` |
| `kinowasm_callinfo_t` | Host callback context: input `args`, output `rets`, `current_store` |
| `kinowasm_extrafunc_t` | Host import definition: `module`, `name`, `func`, `reserved` |

Numeric tags are `TYPE_VAL_I32`, `TYPE_VAL_I64`, `TYPE_VAL_F32`, `TYPE_VAL_F64`. Read/write values through `val.num.i32`, `.i64`, `.f32`, `.f64`. Reference values use `val.ref`, with `REF_NULL` for null. The declared `TYPE_VAL_V128` type does not imply implemented SIMD execution.

## Runtime lifecycle and ownership

| Function | Contract |
|---|---|
| `kinowasm_init()` | Create a store using the active allocation arena; returns a handle |
| `kinowasm_term(store)` | Tear down runtime-owned state; does not free caller-owned backing buffers |
| `kinowasm_assign_memory(store, memory, size)` | Install a caller-provided store arena; call before loading |
| `kinowasm_get_memory(store)` | Return the assigned store arena descriptor |
| `kinowasm_reset_store(store)` | Reset store contents; reacquire function handles before subsequent calls |
| `kinowasm_set_stack_size(size)` | Configure stack sizing |
| `kinowasm_set_object_cache_size(size)` | Configure object cache sizing |

Initialize an active system arena before creating stores or registering imports. The reference `host/systemmemory.c` provides `change_system_memory()`. Store and module backing memory must remain valid until the runtime no longer uses it. Destroy runtime state before freeing those buffers. A host-provided linear-memory backend is a separate requirement; `host/kw_core_mem_backend_win.h` supplies the Windows implementation.

These APIs do not make arbitrary null handles or invalid backing buffers safe. Follow the documented lifecycle and check allocation failures in the caller.

## Host registration

```c
kinowasm_result_t kinowasm_register_extra_func(
	const kinowasm_extrafunc_t* extra, size_t size);
void kinowasm_clear_extra_func(void);
```

`size` is the number of entries, not a byte count. Register imports before loading modules that require them. Entries are appended to a global registry; clearing it affects global host registration state. Synchronize use of global state rather than assuming separate stores provide thread isolation.

A callback has signature `kinowasm_result_t callback(kinowasm_callinfo_t* call)`. Set both the type tag and value of each return slot. `reserved` may carry the reference host layer's signature string; this is not a general guarantee that registration alone validates every callback contract. See [Host functions](Host-Functions.md).

## Loading

```c
kinowasm_result_t kinowasm_load_module(
	kinowasm_handle_t store, const char* modulefile,
	const char* modulename, kinowasm_mem_info_t modulememory);
kinowasm_result_t kinowasm_load_module_from_memory(
	kinowasm_handle_t store, void* module_data, size_t module_size,
	const char* modulename, kinowasm_mem_info_t modulememory);
```

Provide a valid module arena and a name used for later lookup/import resolution. Imports must already resolve to registered host functions or loaded modules. The memory-loading function needs valid input bytes throughout the load; module arena ownership remains with the caller. Keep arena buffers alive until teardown.

## Lookup and invocation

| Function | Use |
|---|---|
| `kinowasm_invoke(store, module, funcname, argument)` | Resolve a null-terminated export name and invoke |
| `kinowasm_invoke_n(store, module, funcname, funcname_len, argument)` | Invoke with an explicit export-name byte length |
| `kinowasm_lookup_func(store, module, funcname, out)` | Resolve a function once for repeated calls |
| `kinowasm_lookup_func_n(store, module, funcname, funcname_len, out)` | Resolve with an explicit export-name byte length |
| `kinowasm_invoke_func(store, func, argument)` | Invoke a resolved handle without name lookup |
| `kinowasm_resume(store, argument)` | Continue a suspended invocation |

The argument array is **input/output**: supply typed parameters before invoking; successful results replace its contents. Check the result before reading return values. Use the `_n` forms for export names containing embedded NUL bytes; module names remain null-terminated strings.

Resolved handles belong to their store and remain usable only while their originating module is loaded. Reacquire them after resetting or reloading. Lookup failures set the output handle to the invalid value. Missing modules/functions and invalid handles have distinct error results; see [Error handling](Error-Handling.md).

Suspension preserves execution state. Resume only the corresponding suspended invocation, after preparing any host return values required by that operation. A non-success result can also be an ordinary trap or validation failure; do not resume indiscriminately.

## WASM memory access

```c
kinowasm_result_t kinowasm_read_memory(
	kinowasm_callinfo_t* call, uint32_t start_address,
	void* data, size_t len);
kinowasm_result_t kinowasm_write_memory(
	kinowasm_callinfo_t* call, uint32_t start_address,
	const void* data, size_t len);
```

Use the callback context supplied by the runtime. Addresses are WASM linear-memory offsets, not native pointers. Check every transfer result before using data. Success means all `len` bytes were transferred; unavailable/out-of-range memory returns `ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS`, and allocation failure may return `ERR_OUTOFMEMORY`. Do not fabricate a callback context for general out-of-call access.

In the current core host bridge `current_store` is null; the memory functions use the active core context. Do not dereference or assume a non-null store field in a callback.

## Allocation helpers

`kinowasm_mem_info_init(memory, len)` initializes an arena. `kinowasm_mem_get_info()` / `kinowasm_mem_set_info(info)` query/select the active arena. `kinowasm_mem_init()` / `kinowasm_mem_cleanup()` manage allocator state.

Allocation functions are `kinowasm_mem_malloc_aligned`, `kinowasm_mem_calloc_aligned`, `kinowasm_mem_realloc_aligned`, and `kinowasm_mem_free`; default-alignment wrappers are `kinowasm_mem_malloc`, `kinowasm_mem_calloc`, `kinowasm_mem_realloc`. Check returned pointers. Statistics/maintenance helpers are `kinowasm_mem_used_size`, `kinowasm_mem_unuse_size`, `kinowasm_mem_total_size`, and `kinowasm_mem_merge`.

Do not mix arena allocations with native `free()`. Native backing buffers belong to the application; individual allocations inside them belong to the arena allocator.

## Argument arrays

```c
kinowasm_args_t args = { 0 };
kinowasm_result_t result = kinowasm_array_new_from(args, 1);
if(result == RES_SUCCESS) {
	args.data[0].type = TYPE_VAL_I32;
	args.data[0].val.num.i32 = 40;
	result = kinowasm_invoke(store, "sample", "fibonacci", &args);
}
kinowasm_array_term_from(args);
```

The snippet assumes an initialized allocator and a loaded `sample` module. `kinowasm_array_init_from` initializes an empty descriptor; `kinowasm_array_new_from` allocates elements; `kinowasm_array_grow_from` grows storage; `kinowasm_array_copy_from` copies contents; `kinowasm_array_term_from` releases storage. Always check operations returning a result code.

Related: [Quick guide](QuickGuide.md), [Host functions](Host-Functions.md), [Error handling](Error-Handling.md).
