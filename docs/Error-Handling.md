# Error handling

English | [日本語](ja/Error-Handling.md)

Public operations returning `kinowasm_result_t` must be checked against `RES_SUCCESS`. Runtime errors are declared in `KinoWASM/exceptioncode.h`; allocator/common results and internal error macros are in `KinoWASM/KinoUtil/exception.h`.

## Application-side handling

```c
kinowasm_result_t result = kinowasm_load_module(store, path, "sample", module_memory);
if(result != RES_SUCCESS) {
	fprintf(stderr, "load failed: %u\n", (unsigned)result);
	/* Tear down runtime state before freeing its backing buffers. */
	return 1;
}
```

The excerpt assumes its caller owns cleanup. Do not read invocation results after a failed call, ignore memory transfer failures, or free backing memory while runtime state still refers to it.

## Common runtime errors

| Symbol | Meaning |
|---|---|
| `ERR_FILENOTOPEN` | Input could not be opened or was unavailable |
| `ERR_MAGICNOTDETECT`, `ERR_VERSIONMISMATCH` | Invalid module header/version |
| `ERR_UNKNOWN_IMPORT_SYMBOL`, `ERR_UNKNOWN_IMPORT` | Missing module/import/export resolution |
| `ERR_INCOMPATIBLE_IMPORT_TYPE` | Import type mismatch |
| `ERR_INVALID_FUNC_PARAM` | Invalid function handle/parameters |
| `ERR_OUTOFMEMORY` | Allocation failed |
| `ERR_STACK_OVERFLOW` | Stack budget exhausted |
| `ERR_TRAP_UNREACHABLE` | Guest executed `unreachable` |
| `ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS` | Guest memory or host transfer range is invalid |
| `ERR_TRAP_OUT_OF_BOUNDS_TABLE_ACCESS` | Table index out of bounds |
| `ERR_TRAP_INDIRECT_CALL_TYPE_MISMATCH` | Indirect call signature mismatch |
| `ERR_TRAP_UNINITIALIZED_ELEMENT` | Uninitialized function table element |
| `ERR_TRAP_INTERGER_DIVIDE_BY_ZERO` | Integer divide by zero |
| `ERR_TRAP_INTERGET_OVERFLOW` | Integer overflow trap |
| `ERR_TRAP_INVALID_CONVERSION_TO_INTERGER` | Invalid float-to-integer conversion |
| `ERR_NOT_RESUME`, `ERR_FUNCTION_ALREADY_SUSPENDED` | Invalid suspension/invocation lifecycle |
| `ERR_TRAP_UNALIGNED_ATOMIC` | Unaligned atomic access |
| `ERR_ATOMIC_WAIT_NON_SHARED` | Atomic wait on non-shared memory |
| `ERR_TRAP_UNCAUGHT_EXCEPTION` | Uncaught WASM exception |

Some names retain historical spelling; use the declared identifiers rather than correcting their spelling in application code. This table is not exhaustive. Consult the headers for all validation and proposal-specific results instead of relying on numeric ranges.

## Host errors and suspension

Host callbacks return result codes, too. The shared host layer has its own errors, including `INVALID_ARGUMENT`, `ERR_SEGMENTATION_FAULT`, and `ERR_ALIGNMENT_FAULT`. Guest WASI errno values are a separate channel from runtime result codes.

Not every non-success result is resumable. Only resume a deliberately suspended operation whose protocol your application understands. Keep its store/frame state intact until completion or teardown. See [Host functions](Host-Functions.md#suspension-and-module-lifecycle).

## Internal macros

`_try`, `_catch`, `_throw`, `_throwif` and `_throwiferr` implement the internal result propagation convention. They are implementation helpers rather than a requirement for calling the public API. Exceptional internal paths can also use `setjmp` / `longjmp`; do not assume application stack cleanup runs like C++ exception unwinding.

## Reporting a failure

Record the module/input, invocation arguments, result code, compiler, preset and repository revision. For tests, include the failing JSON and its output. A case producing no `passed=` result is a crash, not a successful test with zero assertions. Separate diagnostics on stderr when aggregating testsuite counts.

Related: [Public API](Public-API.md), [Testing](Testing.md).
