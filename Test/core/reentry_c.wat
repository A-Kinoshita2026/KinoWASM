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
  (func (export "divzero") (result i32)
    i32.const 1 i32.const 0 i32.div_s)
  (func (export "oob") (result i32)
    i32.const 65536 i32.load)
  (func (export "thrower") (result i32)
    throw $error))
