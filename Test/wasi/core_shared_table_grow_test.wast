;; core_shared_table_grow_test.wast
;;
;; Regression test for cross-module shared-table `table.grow` pointer/size sync.
;;
;; Module $tabowner owns and exports a funcref table; module $tabuser imports it
;; and grows it. Before the fix, `table.grow` (H_table_grow) only updated the
;; growing instance's view of the shared table (the `data` pointer and `size`);
;; the owner kept a STALE pointer (use-after-free after realloc moves the buffer)
;; and a STALE size (too small -> call_indirect on the grown range traps).
;;
;; The fix (kw_core_sync_shared_table_grow) propagates the realloc'd pointer and
;; new size to every core instance that shares the same store_tableaddr, mirroring
;; the existing kw_core_sync_shared_mem_grow for linear memory.
;;
;; This test asserts the owner observes the grown size (size sync) and can still
;; call the pre-grow element after the importer's grow (data pointer validity /
;; no use-after-free). The pointer/size sync is exactly what kw_core_sync_shared_
;; table_grow restores; the size assertion fails deterministically without it
;; (the owner's stale size makes call_indirect on the grown range trap).

(module $tabowner
  (type $v_i32 (func (result i32)))
  (table (export "shared_table") 1 100 funcref)
  (func $ret100 (result i32) (i32.const 100))
  (elem (i32.const 0) func $ret100)
  (func (export "call_at") (param $i i32) (result i32)
    (call_indirect (type $v_i32) (local.get $i)))
  (func (export "size") (result i32) (table.size 0)))
(register "tabowner" $tabowner)

(module $tabuser
  (import "tabowner" "shared_table" (table 1 100 funcref))
  (func (export "grow") (param $delta i32) (result i32)
    (table.grow 0 (ref.null func) (local.get $delta))))
(register "tabuser" $tabuser)

;; pre-grow: owner sees size 1 and element 0 returns 100.
(assert_return (invoke $tabowner "size") (i32.const 1))
(assert_return (invoke $tabowner "call_at" (i32.const 0)) (i32.const 100))

;; importer grows the shared table by 49 (old size 1 -> new size 50); returns old size 1.
(assert_return (invoke $tabuser "grow" (i32.const 49)) (i32.const 1))

;; owner must observe the grown size (size sync); was stale (1) before the fix.
(assert_return (invoke $tabowner "size") (i32.const 50))

;; owner can still call the pre-grow element 0 (data pointer valid after realloc; no UAF).
(assert_return (invoke $tabowner "call_at" (i32.const 0)) (i32.const 100))
