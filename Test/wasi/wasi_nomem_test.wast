;; wasi_nomem_test.wast
;;
;; Regression test for host-function calls from a module WITHOUT linear memory
;; (kinowasm.c kinowasm_read_memory / kinowasm_write_memory).
;;
;; A module that declares no memory can still import and call WASI functions.
;; kw_core_invoke_host then runs the host function with g_core_host_mem == NULL
;; and call->current_store == NULL. Before the fix, kinowasm_read/write_memory
;; fell through to the legacy store path and dereferenced the NULL store
;; (GET_CURRENT_MEMORY -> store->stack), crashing the host with an access
;; violation (0xC0000005) -- a one-module denial of service. The fix rejects
;; the transfer before the store path, so fd_write's attempt to write the
;; nwritten result now fails cleanly and returns EFAULT (21).

(module
  (import "wasi_snapshot_preview1" "fd_write" (func $fdw (param i32 i32 i32 i32) (result i32)))
  (func (export "nomem") (result i32)
    (call $fdw (i32.const 1) (i32.const 0) (i32.const 1) (i32.const 8)))
)
(assert_return (invoke "nomem") (i32.const 21))
