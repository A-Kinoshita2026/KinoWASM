;; core_trytable_carry_test.wast
;;
;; Regression tests for value carry into try_table labels and for
;; call_indirect inside try blocks (2026-07-10).
;;
;; (1) arity-1 br/br_if/br_table targeting a try_table label: the end
;;     continuation of try_table reads results from home slots, but branches
;;     delivered the value in r0 (emit_branch's home_tgt covered loop and
;;     legacy try only), so the value was lost and an uninitialized slot
;;     (usually 0) was returned (kw_core_compile.c).
;; (2) throw_ref with a live computed value (r0) below the try_table: the
;;     block-entry spill keeps values below sp_base out of r0, so the catch
;;     dispatch cannot clobber them (kw_core_compile.c enter_block).
;; (3) call_indirect inside try/try_table must decode foreign funcrefs
;;     (-2-fa encoding) in non-shared tables like the non-EH variants do;
;;     H_call_indirect_eh was missing the decode and trapped with
;;     "uninitialized element" (kw_core_exec.c).

(module
  (tag $t)
  (func (export "brval") (result i32)
    (try_table (result i32) (br 0 (i32.const 42))))
  (func (export "brifval") (param i32) (result i32)
    (try_table (result i32)
      (i32.const 7)
      (br_if 0 (local.get 0))
      (drop) (i32.const 9)))
  (func (export "brtval") (param i32) (result i32)
    (try_table (result i32)
      (i32.const 5)
      (br_table 0 0 (local.get 0))))
  (func (export "throwrefspill") (result i32)
    (local $e exnref)
    (block $c (result exnref)
      (try_table (result exnref) (catch_all_ref $c)
        (throw $t))
      (unreachable))
    (local.set $e)
    (i32.const 5) (i32.const 6) (i32.add)   ;; live r0 below the next block
    (block $b
      (try_table (catch_all $b)
        (local.get $e) (throw_ref))
      (unreachable))
    (i32.const 100) (i32.add))              ;; expect 11+100
)

(assert_return (invoke "brval") (i32.const 42))
(assert_return (invoke "brifval" (i32.const 1)) (i32.const 7))
(assert_return (invoke "brifval" (i32.const 0)) (i32.const 9))
(assert_return (invoke "brtval" (i32.const 0)) (i32.const 5))
(assert_return (invoke "throwrefspill") (i32.const 111))

;; (3) foreign funcref moved into a non-shared table, called inside try.
(module $A2
  (func $f0 (result i32) (i32.const 100))
  (table (export "tab") 8 funcref)
  (elem (i32.const 0) $f0)
)
(register "A2" $A2)
(module $B2
  (type $ft (func (result i32)))
  (import "A2" "tab" (table $t 8 funcref))
  (table $loc 4 funcref)
  (func (export "movex")
    (table.set $loc (i32.const 0) (table.get $t (i32.const 0))))
  (func (export "calleh") (result i32)
    (try (result i32)
      (do (call_indirect $loc (type $ft) (i32.const 0)))
      (catch_all (i32.const -1))))
  (func (export "calleh2") (result i32)
    (block $h
      (return (try_table (result i32) (catch_all $h)
        (call_indirect $loc (type $ft) (i32.const 0)))))
    (i32.const -1))
)
(invoke "movex")
(assert_return (invoke "calleh") (i32.const 100))
(assert_return (invoke "calleh2") (i32.const 100))
