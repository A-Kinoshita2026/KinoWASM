;; core_eh_branch_test.wast  (requires wast2json --enable-exceptions)
;;
;; Regression tests for value carrying into exception-handling constructs
;; (register-TOS core compiler, kw_core_compile.c).
;;
;; brtry: a legacy `try` block's end receives its result from HOME SLOTS
;; (block_end_common / the legacy-try end path), unlike plain blocks whose
;; arity-1 result travels in r0. Before the fix, emit_branch treated legacy try
;; like a plain block and sent the arity-1 value in r0, so `br 0` into a legacy
;; try returned an uninitialized slot (0) instead of 7. The fix routes
;; home-slot-receiving targets (loops AND legacy try) through the home-slot
;; carry path (see also core_loop_carry_test.wast).
;;
;; catchblk: documents the H_catch calling convention -- catch handlers deliver
;; tag params BOTH to the target's home slots and (for arity 1) in r0, so a
;; catch clause may target either a home-slot receiver (loop/legacy try) or an
;; r0 receiver (block). This was verified correct during the same audit; the
;; test pins the convention against future changes.

(module
  (func (export "brtry") (result i32)
    (try (result i32)
      (do
        (br 0 (i32.const 7))
      )
      (catch_all (i32.const 9))
    )
  )
)
(assert_return (invoke "brtry") (i32.const 7))

(module
  (tag $e (param i32))
  (func (export "catchblk") (result i32)
    (block $B (result i32)
      (try_table (result i32) (catch $e $B)
        (throw $e (i32.const 42))
        (i32.const 0)
      )
    )
  )
)
(assert_return (invoke "catchblk") (i32.const 42))
