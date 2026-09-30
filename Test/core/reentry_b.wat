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
  (export "bfun" (func $bfun)))
