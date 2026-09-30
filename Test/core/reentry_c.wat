;; reentry_c.wat - re-entry target for kw_core_reentrytest (loaded as "ModC").
;; The host callback invokes "cfun" through the public API while ModB is still
;; executing inside a cross-module call.
(module
  (type (;0;) (func (result i32)))
  (func $cfun (type 0) (result i32)
    i32.const 123)
  (export "cfun" (func $cfun)))
