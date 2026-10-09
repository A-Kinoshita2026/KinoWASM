;; reentry_c.wat - re-entry target for kw_core_reentrytest (loaded as "ModC").
;; The host callback invokes "cfun" through the public API while ModB is still
;; executing inside a cross-module call.
(module
  (type (;0;) (func (result i32)))
  (func $cfun (type 0) (result i32)
    i32.const 123)
  (export "cfun" (func $cfun))
  (memory 1)
  (tag $error)
  (tag $large (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32))
  (func (export "throw_large") (result i32)
    i32.const 1 i32.const 2 i32.const 3 i32.const 4 i32.const 5 i32.const 6
    i32.const 7 i32.const 8 i32.const 9 i32.const 10 i32.const 11 i32.const 12
    i32.const 13 i32.const 14 i32.const 15 i32.const 16 i32.const 99 throw $large)
  (func (export "divzero") (result i32)
    i32.const 1 i32.const 0 i32.div_s)
  (func (export "oob") (result i32)
    i32.const 65536 i32.load)
  (func (export "thrower") (result i32)
    throw $error))
