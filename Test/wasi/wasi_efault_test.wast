;; wasi_efault_test.wast
;;
;; Regression: WASI out-pointer write failures must return EFAULT (21), not
;; ESUCCESS (0) (extrafunction.c, 2026-07-10).
;;
;; 26 kinowasm_write_memory call sites ignored the return value and returned
;; ESUCCESS even when the guest-supplied out pointer was outside linear memory,
;; silently dropping the result. They were unified to return __WASI_EFAULT (21)
;; on write failure. Here each out pointer is 0xFFFF0000, far past a 1-page
;; (64 KiB) memory, so the write must fail and the call must report EFAULT.
;;
;; Prereq: testsuite_runner registers the standard WASI funcs at startup.

(module
  (import "wasi_snapshot_preview1" "clock_time_get"
    (func $clock_time_get (param i32 i64 i32) (result i32)))
  (import "wasi_snapshot_preview1" "clock_res_get"
    (func $clock_res_get (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "random_get"
    (func $random_get (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "args_sizes_get"
    (func $args_sizes_get (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "environ_sizes_get"
    (func $environ_sizes_get (param i32 i32) (result i32)))
  (memory (export "memory") 1)

  ;; out pointer past the end of a 1-page memory -> EFAULT (21)
  (func (export "clock_time_oob") (result i32)
    (call $clock_time_get (i32.const 0) (i64.const 0) (i32.const 0xFFFF0000)))
  (func (export "clock_res_oob") (result i32)
    (call $clock_res_get (i32.const 0) (i32.const 0xFFFF0000)))
  (func (export "random_oob") (result i32)
    (call $random_get (i32.const 0xFFFF0000) (i32.const 16)))
  (func (export "args_sizes_oob") (result i32)
    (call $args_sizes_get (i32.const 0xFFFF0000) (i32.const 0)))
  (func (export "environ_sizes_oob") (result i32)
    (call $environ_sizes_get (i32.const 0xFFFF0000) (i32.const 0)))

  ;; sanity: valid out pointer -> ESUCCESS (0)
  (func (export "clock_time_ok") (result i32)
    (call $clock_time_get (i32.const 0) (i64.const 0) (i32.const 0)))
  (func (export "random_ok") (result i32)
    (call $random_get (i32.const 0) (i32.const 16))))

(assert_return (invoke "clock_time_oob") (i32.const 21))
(assert_return (invoke "clock_res_oob") (i32.const 21))
(assert_return (invoke "random_oob") (i32.const 21))
(assert_return (invoke "args_sizes_oob") (i32.const 21))
(assert_return (invoke "environ_sizes_oob") (i32.const 21))
(assert_return (invoke "clock_time_ok") (i32.const 0))
(assert_return (invoke "random_ok") (i32.const 0))
