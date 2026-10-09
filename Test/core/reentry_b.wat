;; reentry_b.wat - middle module for kw_core_reentrytest (loaded as "ModB").
;; ModA calls $bfun through a cross-module import. Depending on $mode, $bfun
;;   mode 0: calls the host, which re-enters the public API (ModC)
;;   mode 1: suspends once through the yielding host import
;;   mode 2: suspends once, then re-enters the public API after the resume
;; and finally calls its own $inner. If the engine loses track of which
;; instance is executing, that last call resolves against the wrong module's
;; function table.
;; $inner reads the answer (7) out of ModB's own linear memory, so restoring
;; the wrong instance's mem is caught too, not just the wrong function table.
(module
  (import "env" "hostcb" (func $cb (param i32) (result i32)))
  (import "env" "yhost" (func $y (param i32) (result i32)))
  (memory 1)
  (data (i32.const 0) "\07\00\00\00")
  (func $inner (param $x i32) (result i32)
    (i32.load (i32.const 0)))
  (func $bfun (param $mode i32) (result i32)
    (if (i32.ge_s (local.get $mode) (i32.const 1))
      (then (drop (call $y (i32.const 0)))))
    (if (i32.ne (local.get $mode) (i32.const 1))
      (then (drop (call $cb (i32.const 0)))))
    (call $inner (i32.const 0)))
  (export "bfun" (func $bfun))
  (tag $error)
  (tag $large (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32))
  (func $throw_large (export "throw_large") (result i32)
    i32.const 1 i32.const 2 i32.const 3 i32.const 4 i32.const 5 i32.const 6
    i32.const 7 i32.const 8 i32.const 9 i32.const 10 i32.const 11 i32.const 12
    i32.const 13 i32.const 14 i32.const 15 i32.const 16 i32.const 42 throw $large)
  (func (export "large_reentry") (param $mode i32) (result i32) (local $last i32)
    try (result i32)
      try (result i32)
        call $throw_large
      catch_all
        local.get $mode
        if i32.const 0 call $y drop end
        i32.const 0 call $cb drop
        rethrow 0
      end
    catch $large
      local.set $last
      drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop
      local.get $last
    end)
  (func (export "divzero") (result i32)
    i32.const 1 i32.const 0 i32.div_s)
  (func (export "oob") (result i32)
    i32.const 65536 i32.load)
  (func (export "thrower") (result i32)
    throw $error))
