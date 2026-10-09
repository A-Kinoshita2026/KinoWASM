;; A different module must receive the exception, not its stale yield message.
(module
  (import "ModuleEH" "yield_throw" (func $throw (result i32)))
  (import "ModuleEH" "trap_child" (func $trap (result i32)))
  (import "ModuleEH" "get_ref_yield" (func $get_ref (result funcref)))
  (type $int (func (result i32)))
  (table 1 funcref)
  (func (export "ref_yield") (result i32)
    i32.const 0 call $get_ref table.set
    i32.const 0 call_indirect (type $int))
  (tag $other)
  (func (export "nested_throw") (result i32) call $throw)
  (func (export "nested_trap") (result i32) call $trap)
  (func (export "unmatched_throw") (result i32)
    try (result i32)
      call $throw
    catch $other
      i32.const 99
    end))
