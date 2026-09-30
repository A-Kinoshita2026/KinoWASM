;; eh_yield_test.wat - input for kw_core_yieldtest part 2 (EH x suspend/resume).
;;
;; "run_eh" calls $g inside a try_table. $g calls the yielding host import
;; (suspend point) and, after the resume, throws tag $e carrying
;; yield_result + 7. The exception must unwind $g's resumed frame and be
;; caught by run_eh's catch clause, which returns the carried value.
;;
;; Expected: run_eh() = 17 (host returns 10 on the first call), 1 yield.

(module
  (import "env" "yield" (func $yield (result i32)))
  (tag $e (param i32))
  (func $g (result i32)
    (throw $e (i32.add (call $yield) (i32.const 7))))
  (func (export "run_eh") (result i32)
    (block $h (result i32)
      (try_table (result i32) (catch $e $h)
        (call $g)))))
