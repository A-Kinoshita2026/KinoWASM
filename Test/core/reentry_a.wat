;; reentry_a.wat - outer module for kw_core_reentrytest (loaded as "ModA").
;; $poison0 / $poison1 occupy defined-function indices 0 and 1 so that a call
;; mis-resolved against ModA's table returns 999 / 998 instead of failing
;; silently. ModA's memory holds 999 at offset 0 for the same reason: if
;; ModB's $inner runs against ModA's linear memory it reads that instead of 7.
(module
  (import "ModB" "bfun" (func $b (param i32) (result i32)))
  (memory 1)
  (data (i32.const 0) "\e7\03\00\00")
  (func $poison0 (param $x i32) (result i32)
    i32.const 999)
  (func $poison1 (param $x i32) (result i32)
    i32.const 998)
  (func $afun (param $mode i32) (result i32)
    (call $b (local.get $mode)))
  (export "afun" (func $afun)))
