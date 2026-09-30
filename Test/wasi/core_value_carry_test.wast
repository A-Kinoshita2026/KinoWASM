;; core_value_carry_test.wast
;;
;; Regression tests for register-TOS value carry across block boundaries
;; (kw_core_compile.c, 2026-07-10).
;;
;; (1) br to an else-less `if (param) (result)`: its end continuation reads
;;     the result from the home slot (required by the cond==0 pass-through
;;     path), but the br delivered it in r0, so the param snapshot was
;;     returned instead of the branched value. Fixed by giving parameterized
;;     ifs the home-slot convention on both the send and receive sides
;;     (has_else is not yet known when the br is compiled).
;; (2) a live computed value (L_REG, held in r0) left below a block boundary:
;;     end reset the tracking (reg_pos=-1) and a later r0-clobbering
;;     instruction skipped the spill, corrupting the value (g1+g1 instead of
;;     g0+g1). Fixed by spilling at block entry, so values below sp_base
;;     never stay in r0 across a block.

(module
  (global $g0 i32 (i32.const 30))
  (global $g1 i32 (i32.const 12))
  (func (export "ifbr") (param i32) (result i32)
    (i32.const 5)
    (local.get 0)
    (if (param i32) (result i32)
      (then (i32.const 100) (i32.add) (br 0))))
  (func (export "regorphan") (result i32)
    (global.get $g0)
    (block)
    (global.get $g1)
    (i32.add))
)

(assert_return (invoke "ifbr" (i32.const 1)) (i32.const 105))
(assert_return (invoke "ifbr" (i32.const 0)) (i32.const 5))
(assert_return (invoke "regorphan") (i32.const 42))
