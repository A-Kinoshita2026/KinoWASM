;; core_shared_table_gfi_test.wast
;;
;; Regression test for funcref value representation on cross-module shared
;; tables (kw_core_exec.c table ops + kw_core_bridge.c).
;;
;; Core funcref values live in two domains:
;;   - gfi domain (operand stack / locals / non-shared tables): v >= 0 is the
;;     executing module's global func index (what `ref.func` pushes), -1 is
;;     null, v <= -2 encodes a FOREIGN function's store funcaddr as -2-fa.
;;   - funcaddr domain (global_ref shared-table buffers / store elems): store
;;     funcaddr. Shared buffers are read by multiple modules, so module-local
;;     gfi values cannot represent their elements.
;;
;; Before the fix, runtime writes (table.set/fill/init/grow) stored raw gfi
;; values into shared (global_ref) tables, breaking the "shared element =
;; funcaddr" invariant: call_indirect then resolved the gfi AS a funcaddr and
;; called a DIFFERENT module's function ("set2"+"call 2" returned 101 instead
;; of 201). The fix translates at every table read/write boundary and encodes
;; foreign funcaddrs (-2-fa) so get->set round trips and moves into non-shared
;; tables also work (call_indirect decodes v <= -2 via the funcaddr-resolve
;; path; the hot gfi fast path is unchanged).

(module $A
  (func $f0 (result i32) (i32.const 100))
  (func $f1 (result i32) (i32.const 101))
  (table (export "tab") 8 funcref)
  (elem (i32.const 0) $f0 $f1)
)
(register "A" $A)

(module $B
  (type $ft (func (result i32)))
  (import "A" "tab" (table $t 8 funcref))
  (func $g0 (result i32) (i32.const 200))
  (func $g1 (result i32) (i32.const 201))
  (elem declare func $g1)
  (table $loc 4 funcref)
  (func (export "set2") (table.set $t (i32.const 2) (ref.func $g1)))
  (func (export "call") (param i32) (result i32)
    (call_indirect $t (type $ft) (local.get 0)))
  (func (export "move0to3")
    (table.set $t (i32.const 3) (table.get $t (i32.const 0))))
  (func (export "movex")
    (table.set $loc (i32.const 0) (table.get $t (i32.const 0))))
  (func (export "callloc") (result i32)
    (call_indirect $loc (type $ft) (i32.const 0)))
  (func (export "fill45")
    (table.fill $t (i32.const 4) (ref.func $g1) (i32.const 2)))
)

;; ref.func (gfi) written by table.set must resolve to B's own $g1 (201), not
;; to whatever function owns store funcaddr 1 (A's $f1, 101) as before the fix.
(invoke "set2")
(assert_return (invoke "call" (i32.const 2)) (i32.const 201))

;; get->set round trip on the shared table with a foreign (A-defined) ref.
(invoke "move0to3")
(assert_return (invoke "call" (i32.const 3)) (i32.const 100))

;; Foreign ref moved into B's non-shared local table: stored encoded (-2-fa)
;; and called through call_indirect's funcaddr-resolve fallback.
(invoke "movex")
(assert_return (invoke "callloc") (i32.const 100))

;; table.fill with a ref.func value on the shared table.
(invoke "fill45")
(assert_return (invoke "call" (i32.const 4)) (i32.const 201))

;; A's original elements survive all of the above untouched.
(assert_return (invoke "call" (i32.const 0)) (i32.const 100))
(assert_return (invoke "call" (i32.const 1)) (i32.const 101))
