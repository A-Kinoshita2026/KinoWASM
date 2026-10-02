# Host functions and WASI

English | [日本語](ja/Host-Functions.md)

Host callbacks implement imported WASM functions. Registration and dispatch live in `KinoWASM/kinowasm.c`; the shared reference host functions, including WASI, live in `host/extrafunction.c`.

## Registering imports

An entry contains import module/name strings, a callback pointer and a `reserved` field. Register entries with `kinowasm_register_extra_func(entries, count)` before loading dependent modules. The registry is global and registrations append entries; use `kinowasm_clear_extra_func()` when resetting the complete host environment.

```c
#include "kinowasm.h"
#include "exceptioncode.h"

static kinowasm_result_t add(kinowasm_callinfo_t* call)
{
	if(call == NULL || call->args == NULL || call->rets == NULL)
		return ERR_INVALID_FUNC_PARAM;

	if(call->args->len != 2 || call->rets->len != 1)
		return ERR_INVALID_FUNC_PARAM;

	if(call->args->data[0].type != TYPE_VAL_I32 || call->args->data[1].type != TYPE_VAL_I32)
		return ERR_INVALID_FUNC_PARAM;

	uint32_t a = (uint32_t)call->args->data[0].val.num.i32;
	uint32_t b = (uint32_t)call->args->data[1].val.num.i32;
	call->rets->data[0].type = TYPE_VAL_I32;
	call->rets->data[0].val.num.i32 = (int32_t)(a + b);
	return RES_SUCCESS;
}

static const kinowasm_extrafunc_t imports[] = {
	{ "env", "add", add, NULL }
};
```

The example assumes the runtime supplies correctly allocated argument/return arrays. Call registration after initializing the active allocator. A WASM import must declare the matching signature, `i32(i32, i32)`. Set both the tag and value of each return slot and propagate failures instead of returning an uninitialized value.

## Signature strings

The reference host layer uses `reserved` for signature strings: the first character specifies the result, and remaining characters specify parameters. `v` means void, `i` i32, `j` i64, `f` f32 and `d` f64. For example, `vi` means `void(i32)` and `iii` means `i32(i32, i32)`. A colon separates alternatives.

`validate_function_parameter` applies these strings in the reference host module invocation path. `NULL` skips this extra validation. Do not assume all custom/public invocation paths apply this host-layer check; validate callback contracts appropriate to your integration.

## Callback context and memory

`call->args` holds parameters, and `call->rets` holds result slots. The current core bridge supplies a null `current_store` field; do not dereference it. During a callback, memory transfer helpers use the active core context.

Use `kinowasm_read_memory(call, offset, buffer, length)` and `kinowasm_write_memory(call, offset, buffer, length)`. Check their results before consuming or acknowledging data. Validate application-specific lengths, encodings, indices and enums in addition to memory bounds. A successfully transferred range does not establish that its contents are valid for a host API.

## WASI reference implementation

`register_standard_func()` in `host/extrafunction.c` registers the shared host functions. The CLI configures the reference environment; a custom embedder must arrange that setup itself. Inspect `host/extrafunction.c` for the current registration table and configuration functions.

The implementation includes argument/environment access, time, basic standard I/O, preopened filesystem operations, and a replaceable VFS through `wasi_set_vfs()`. Project regressions cover read/write, positional I/O, directory operations and link/symlink handling. This is partial WASI support, not complete POSIX compatibility. Socket APIs and `poll_oneoff` are outside the documented supported subset.

WASI guest errors such as `EFAULT` are reported through the guest errno result; runtime traps and host dispatch failures use `kinowasm_result_t`. Configure preopened directories and other capabilities deliberately. The availability of a host function determines what the guest can access.

The minimal WASI fallback in `KinoWASM/core/kw_core_wasi.c` serves standalone drivers. It is not a replacement for the complete shared host filesystem implementation.

## Suspension and module lifecycle

A callback returns `RES_SUCCESS` for normal completion. An intentional non-success result can suspend invocation; the embedding application must distinguish its suspend signals from traps and other failures. Preserve the runtime state, complete the external operation, and call `kinowasm_resume(store, &args)` to continue immediately after the suspended host call. The callback is not invoked again for that call.

The `args` array receives the completed export's results; it does not inject new values into a suspended callback's return slots. Those values are captured when the callback suspends. For asynchronous data, use a void wait import followed by a separate result-reading import after resumption. Callback contexts and their pointers must not be retained past the callback.

See the [cooperative execution example](../examples/cooperative/README.md) for frame-by-frame scheduling and Unity/Unreal templates. This is cooperative scheduling, not automatic preemption: guest code must reach a yielding import, and callbacks must avoid blocking. The example serializes one active session on one thread because the runtime uses global execution/registration state.

The shared host layer additionally exposes `create_wasm_module`, `push_wasm_module`, `invoke_wasm_module`, `pop_wasm_module`, and `destroy_wasm_module`. These are application-side helpers, not functions declared by `kinowasm.h`. Modules pushed through this layer are popped in LIFO order. Check every result and destroy the module context after use. See `Test/pushpop_test.c` for the lifecycle regression.

Related: [Public API](Public-API.md), [Embedding guide](QuickGuide.md), [Error handling](Error-Handling.md), [Testing](Testing.md).
