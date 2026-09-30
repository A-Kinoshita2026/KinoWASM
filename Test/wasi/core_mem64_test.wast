;; core_mem64_test.wast
;;
;; Regression tests for memory64 / table64 address handling (2026-07-10).
;;
;; The core engine used to truncate all addresses/indices to u32, so a
;; memory64 address (or table64 index) >= 2^32 wrapped around and accessed
;; the wrong location instead of trapping, and memory.grow/table.grow
;; interpreted a 2^32 delta as 0 (successful no-op). Fixed by routing
;; memory64 load/store through the memN path (u64 offset + full-width
;; address), a compile-time idx64_chk before call_indirect on table64, and
;; is_64-aware cold ops (grow/copy/fill/init/table.*).

(module
  (memory i64 1)
  (func (export "st") (param i64 i32)
    (i32.store (local.get 0) (local.get 1)))
  (func (export "ld") (param i64) (result i32)
    (i32.load (local.get 0)))
  (func (export "ld_off") (param i64) (result i32)
    (i32.load offset=4 (local.get 0)))
  (func (export "size") (result i64) (memory.size))
  (func (export "grow") (param i64) (result i64)
    (memory.grow (local.get 0)))
  (func (export "fill") (param i64 i32 i64)
    (memory.fill (local.get 0) (local.get 1) (local.get 2)))
)

(assert_return (invoke "st" (i64.const 8) (i32.const 42)))
(assert_return (invoke "ld" (i64.const 8)) (i32.const 42))
(assert_return (invoke "ld_off" (i64.const 4)) (i32.const 42))
(assert_return (invoke "size") (i64.const 1))

;; 2^32 + 8 used to wrap to 8 (read 42); must trap.
(assert_trap (invoke "ld" (i64.const 4294967304)) "out of bounds memory access")
(assert_trap (invoke "st" (i64.const 4294967304) (i32.const 7)) "out of bounds memory access")
;; offset + address that only overflows past 32 bits must trap, not wrap.
(assert_trap (invoke "ld_off" (i64.const 4294967292)) "out of bounds memory access")

;; memory.grow with delta 2^32 must fail (-1), not be treated as 0 (success).
(assert_return (invoke "grow" (i64.const 4294967296)) (i64.const -1))

;; memory.fill with a wrapping destination must trap.
(assert_trap (invoke "fill" (i64.const 4294967296) (i32.const 1) (i64.const 4)) "out of bounds memory access")

;; table64: call_indirect / table.get with an index >= 2^32 must trap, and
;; table.grow with a 2^32 delta must fail.
(module
  (type $ft (func (result i32)))
  (table $t i64 4 funcref)
  (func $f (result i32) (i32.const 7))
  (elem (table $t) (i64.const 1) func $f)
  (func (export "call") (param i64) (result i32)
    (call_indirect $t (type $ft) (local.get 0)))
  (func (export "get_null") (param i64) (result i32)
    (ref.is_null (table.get $t (local.get 0))))
  (func (export "tsize") (result i64) (table.size $t))
  (func (export "tgrow") (param i64) (result i64)
    (table.grow $t (ref.null func) (local.get 0)))
)

(assert_return (invoke "call" (i64.const 1)) (i32.const 7))
(assert_return (invoke "tsize") (i64.const 4))
(assert_trap (invoke "call" (i64.const 4294967297)) "undefined element")
(assert_trap (invoke "get_null" (i64.const 4294967297)) "out of bounds table access")
(assert_return (invoke "tgrow" (i64.const 4294967296)) (i64.const -1))
