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
  (tag $other)
  (tag $large (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32))
  (func $raise_large
    i32.const 1 i32.const 2 i32.const 3 i32.const 4 i32.const 5 i32.const 6
    i32.const 7 i32.const 8 i32.const 9 i32.const 10 i32.const 11 i32.const 12
    i32.const 13 i32.const 14 i32.const 15 i32.const 16 i32.const 42 throw $large)
  (func $last_large (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)
    local.get 16)
  (func (export "large_local") (result i32)
    try (result i32)
      call $raise_large unreachable
    catch $large
      call $last_large
    end)
  (func (export "large_rethrow_yield") (result i32)
    try (result i32)
      try
        call $raise_large
      catch_all
        call $yield drop rethrow 0
      end
      unreachable
    catch $large
      call $last_large
    end)
  (func (export "large_exnref_yield") (result i32) (local exnref)
    try (result i32)
      block $ref (result exnref)
        try_table (catch_all_ref $ref) call $raise_large end
        unreachable
      end
      local.set 0
      call $yield drop
      local.get 0 throw_ref
    catch $large
      call $last_large
    end)
  (func (export "get_ref_yield") (result funcref)
    call $yield drop ref.func $ref_target)
  (func $ref_target (result i32) i32.const 42)
  (elem declare func $ref_target)
  (func (export "plain_throw") (result i32) i32.const 7 throw $e)
  (func $yield_throw (export "yield_throw") (result i32)
    call $yield throw $e)
  (func $parent_throw (result i32) call $yield_throw)
  (func (export "nested_throw") (result i32) call $parent_throw)
  (func (export "unmatched_throw") (result i32)
    try (result i32)
      call $yield_throw
    catch $other
      i32.const 99
    end)
  (func $trap_child (export "trap_child") (result i32)
    call $yield drop unreachable)
  (func $trap_parent (result i32) call $trap_child)
  (func (export "nested_trap") (result i32) call $trap_parent)
  (func (export "yield_ok") (result i32) call $yield drop i32.const 42)
  (func (export "yield_ref") (result i32) (local funcref)
    local.get 0 ref.is_null
    ref.func $yield local.set 0
    call $yield drop
    local.get 0 ref.is_null i32.eqz
    i32.and i32.const 42 i32.mul)
  (elem declare func $yield)
  (func (export "catch_yield") (result i32)
    try (result i32)
      call $yield_throw
    catch_all
      call $yield drop i32.const 42
    end)
  (func $g (result i32)
    (throw $e (i32.add (call $yield) (i32.const 7))))
  (func (export "run_eh") (result i32)
    (block $h (result i32)
      (try_table (result i32) (catch $e $h)
        (call $g)))))
