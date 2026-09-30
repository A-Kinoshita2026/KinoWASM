;; yield_test.wat - input module for kw_core_yieldtest.
;; "run" calls the imported host function "env"."yield" (() -> i32) three
;; times in a loop and returns the running sum. With a host that returns
;; host_calls*10 (10 + 20 + 30) the result is 60 after 3 suspend/resume cycles.
(module
  (type (;0;) (func (result i32)))
  (import "env" "yield" (func $yield (type 0)))
  (func $run (type 0) (result i32)
    (local $acc i32) (local $i i32)
    (loop $L
      local.get $acc
      call $yield
      i32.add
      local.set $acc
      local.get $i
      i32.const 1
      i32.add
      local.set $i
      local.get $i
      i32.const 3
      i32.lt_s
      br_if $L)
    local.get $acc)
  (export "run" (func $run)))
