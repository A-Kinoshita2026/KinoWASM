;; core_eh_invalid_test.wast
;;
;; Regression tests for validation at legacy-EH section boundaries
;; (kw_parser.c / kw_parser_ops_base.inc, 2026-07-10).
;;
;; catch/catch_all/delegate close the try body (or the previous handler) and
;; must validate the result type BEFORE DROP_STACK_TO_POLYMORPHIC: validating
;; after the drop is always relaxed by the polymorphic rule, so type-deficient
;; bodies reached the compiler and crashed with a negative operand-stack index
;; (0xC0000005) instead of being rejected as invalid.

(assert_invalid
  (module (tag $t)
    (func (result i32) (try (result i32) (do (nop)) (catch_all (i32.const 2)))))
  "type mismatch")

(assert_invalid
  (module (tag $t)
    (func (result i32) (try (result i32) (do (nop)) (catch $t (i32.const 1)))))
  "type mismatch")

(assert_invalid
  (module
    (func (result i32)
      (block (result i32)
        (try (result i32) (do (nop)) (delegate 0)))))
  "type mismatch")

;; throw_ref must pop an exnref operand (kw_parser_ops_base.inc, 2026-07-21).
;; The handler only did DROP_STACK_TO_POLYMORPHIC without popping, so a
;; throw_ref on an empty stack (or a non-exnref top) was accepted; the core
;; compiler then did to_slot(cdepth-1) on an empty stack, reading st[-1] and
;; underflowing cdepth -> 0xC0000005.

;; throw_ref on an empty stack
(assert_invalid
  (module (func throw_ref))
  "type mismatch")

;; throw_ref with a non-exnref (i32) operand on top
(assert_invalid
  (module (func (result i32) (i32.const 1) throw_ref))
  "type mismatch")
