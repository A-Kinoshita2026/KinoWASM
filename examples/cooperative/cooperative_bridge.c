#include "cooperative_bridge.h"
#include "kinowasm.h"
#include "exceptioncode.h"
#include "systemmemory.h"
#include "kw_core_mem_backend_win.h"
#include <stdlib.h>
#include <string.h>

#define EXAMPLE_NEXT_FRAME ((kinowasm_result_t)0x2000)

static kinowasm_handle_t store;
static kinowasm_args_t args;
static void* store_buffer;
static void* module_buffer;
static uint8_t* module_bytes;
static kinowasm_result_t last_error;
static int32_t progress;
static int32_t final_result;
static int started;
static int suspended;
static int finished;
static int failed;

/* The sample has no WASI imports; the core fallback still needs these. */
int wasi_get_argc(void)
{
	return 0;
}

const char* const* wasi_get_argv(void)
{
	static const char* const empty[] = { NULL };
	return empty;
}

const char* const* wasi_get_envp(void)
{
	return NULL;
}

static kinowasm_result_t next_frame(kinowasm_callinfo_t* call)
{
	if(call == NULL || call->args == NULL || call->rets == NULL)
		return ERR_INVALID_FUNC_PARAM;

	if(call->args->len != 0 || call->rets->len != 0)
		return ERR_INVALID_FUNC_PARAM;

	return EXAMPLE_NEXT_FRAME;
}

static kinowasm_result_t report_step(kinowasm_callinfo_t* call)
{
	if(call == NULL || call->args == NULL || call->rets == NULL)
		return ERR_INVALID_FUNC_PARAM;

	if(call->args->len != 1 || call->rets->len != 0 || call->args->data[0].type != TYPE_VAL_I32)
		return ERR_INVALID_FUNC_PARAM;

	progress = call->args->data[0].val.num.i32;
	return RES_SUCCESS;
}

void KINO_EXAMPLE_CALL kino_example_close(void)
{
	/* This DLL owns the entire runtime/registry, not another embedder's state. */
	kinowasm_array_term_from(args);
	if(store != NULL)
		kinowasm_term(store);

	change_system_memory();
	kinowasm_clear_extra_func();
	free(module_bytes);
	free(module_buffer);
	free(store_buffer);
	store = NULL;
	module_bytes = NULL;
	module_buffer = NULL;
	store_buffer = NULL;
	started = 0;
	suspended = 0;
	finished = 0;
	failed = 0;
	progress = 0;
	final_result = 0;
}

int32_t KINO_EXAMPLE_CALL kino_example_open(const uint8_t* wasm_data, uint32_t wasm_size)
{
	const size_t store_size = 100 * 1024 * 1024;
	const size_t module_size = 20 * 1024 * 1024;
	const kinowasm_extrafunc_t imports[] = {
		{ "env", "next_frame", next_frame, NULL },
		{ "env", "report_step", report_step, NULL }
	};
	kino_example_close();
	last_error = ERR_INVALID_FUNC_PARAM;
	if(wasm_data == NULL || wasm_size == 0)
		return -1;

	last_error = ERR_OUTOFMEMORY;
	store_buffer = malloc(store_size);
	module_buffer = malloc(module_size);
	module_bytes = malloc(wasm_size);
	if(store_buffer == NULL || module_buffer == NULL || module_bytes == NULL)
		goto failure;

	memcpy(module_bytes, wasm_data, wasm_size);
	kw_core_install_default_mem_backend();
	change_system_memory();
	last_error = kinowasm_register_extra_func(imports, sizeof(imports) / sizeof(imports[0]));
	if(last_error != RES_SUCCESS)
		goto failure;

	store = kinowasm_init();
	if(store == NULL) {
		last_error = ERR_OUTOFMEMORY;
		goto failure;
	}
	kinowasm_assign_memory(store, store_buffer, store_size);
	kinowasm_mem_info_t module_memory = kinowasm_mem_info_init(module_buffer, module_size);
	if(module_memory == NULL) {
		last_error = ERR_OUTOFMEMORY;
		goto failure;
	}
	last_error = kinowasm_load_module_from_memory(store, module_bytes, wasm_size, "demo", module_memory);
	if(last_error != RES_SUCCESS)
		goto failure;

	last_error = kinowasm_array_new_from(args, 0);
	if(last_error != RES_SUCCESS)
		goto failure;

	return 0;

failure:
	kino_example_close();
	return -1;
}

int32_t KINO_EXAMPLE_CALL kino_example_tick(void)
{
	if(store == NULL || failed) {
		if(store == NULL)
			last_error = ERR_NOTMODULEINIT;

		return -1;
	}
	if(finished)
		return 0;

	kinowasm_result_t result;
	if(!started) {
		started = 1;
		result = kinowasm_invoke(store, "demo", "run", &args);
	} else if(suspended) {
		result = kinowasm_resume(store, &args);
	} else {
		last_error = ERR_NOT_RESUME;
		failed = 1;
		return -1;
	}
	/* Only this known application signal is resumable. Never retry every error. */
	suspended = result == EXAMPLE_NEXT_FRAME;
	if(suspended)
		return 1;

	last_error = result;
	if(result != RES_SUCCESS) {
		failed = 1;
		return -1;
	}
	if(args.len != 1 || args.data[0].type != TYPE_VAL_I32) {
		last_error = ERR_INVALID_FUNC_PARAM;
		failed = 1;
		return -1;
	}
	final_result = args.data[0].val.num.i32;
	finished = 1;
	return 0;
}

int32_t KINO_EXAMPLE_CALL kino_example_progress(void)
{
	return progress;
}

int32_t KINO_EXAMPLE_CALL kino_example_result(void)
{
	return final_result;
}

uint32_t KINO_EXAMPLE_CALL kino_example_error(void)
{
	return (uint32_t)last_error;
}
