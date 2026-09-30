;; core_loop_carry_test.wast
;;
;; Regression test for arity-1/multi-value branch value carrying to loop targets
;; (register-TOS core compiler, emit_branch / br_table in kw_core_compile.c).
;;
;; A loop body reads its params from home slots (fb + sp_base). Before the fix,
;; arity-1 back-edges (br / br_if / br_table) only placed the carried value in r0
;; and never wrote the target home slot, so the loop param stayed STALE whenever
;; (a) the sent value sat above residual operands (its home slot differs from the
;; param's), or (b) the sent value was still in r0 (an unspilled operation result).
;; The spec suite passed only because its shapes happened to spill the value into
;; the right home slot as a side effect of computing the branch condition.
;;
;; The fix carries loop-bound arity-1 values to the target home slot
;; (carry_n_to_base), using a taken-side trampoline for br_if/br_table when
;; residual operands exist (a pre-branch copy would corrupt the not-taken path's
;; residual operands -- that corruption is covered by "cased" below).

(module
  ;; casea: br_if back-edge where the sent value (999) sits above a residual
  ;; operand (the current param). Before the fix the taken path left the param
  ;; home slot stale -> returned 5 instead of 999.
  (func (export "casea") (result i32)
    (local $i i32)
    (i32.const 5)
    (loop $L (param i32) (result i32)
      (i32.const 999)
      (local.get $i) (i32.const 1) (i32.add) (local.set $i)
      (local.get $i) (i32.const 3) (i32.lt_u)
      (br_if $L)
      (drop)
    )
  )
  ;; caseb: unconditional br back-edge where the sent value is the result of the
  ;; immediately preceding add (lives in r0, never spilled). Before the fix the
  ;; loop param home slot kept its entry value -> returned 0 instead of 14.
  (func (export "caseb") (result i32)
    (local $i i32)
    (block $B (result i32)
      (i32.const 0)
      (loop $L (param i32) (result i32)
        (local.get $i) (i32.const 1) (i32.add) (local.set $i)
        (local.get $i) (i32.const 3) (i32.ge_u)
        (br_if $B)
        (i32.const 7) (i32.add)
        (br $L)
      )
    )
  )
  ;; casec: br_table back-edge to a loop (arity-1). The uniform r0 path could not
  ;; deliver the value to the loop's home slot -> $save stayed 5 instead of 111.
  ;; The fix routes mixed (loop/block) arity-1 br_table targets through per-target
  ;; trampolines (copyslot for home-slot receivers, getslot for r0 receivers).
  (func (export "casec") (result i32)
    (local $i i32) (local $save i32)
    (block $DONE (result i32)
      (i32.const 5)
      (loop $L (param i32) (result i32)
        (local.set $save)
        (local.get $i) (i32.const 1) (i32.add) (local.set $i)
        (i32.const 111)
        (local.get $i) (i32.const 3) (i32.lt_u)
        (br_table $DONE $L)
      )
    )
    (drop)
    (local.get $save)
  )
  ;; cased: multi-value (arity>=2) br_if whose sent values sit above residual
  ;; operands. Before the fix carry_n_to_base ran BEFORE the branch, so the
  ;; NOT-TAKEN path saw its residual operands' home slots overwritten by the
  ;; carried values (a=1 became 10). The fix moves the carry into a taken-side
  ;; trampoline. The branch here is never taken (i < 0 is always false), so this
  ;; asserts the not-taken path's operands survive intact.
  (func (export "cased") (result i32)
    (local $i i32)
    (i32.const 1) (i32.const 2)
    (loop $L (param i32 i32) (result i32)
      ;; [a, b]
      (i32.const 99)
      (i32.const 10) (i32.const 20)
      (local.get $i) (i32.const 1) (i32.add) (local.set $i)
      (local.get $i) (i32.const 0) (i32.lt_u)          ;; always 0 -> not taken
      (br_if $L)
      ;; not-taken: [a, b, 99, 10, 20]
      (drop) (drop) (drop) (drop)
      ;; [a]
    )
  )
)

(assert_return (invoke "casea") (i32.const 999))
(assert_return (invoke "caseb") (i32.const 14))
(assert_return (invoke "casec") (i32.const 111))
(assert_return (invoke "cased") (i32.const 1))
