;; yield_test.wat - input module for kw_core_yieldtest.
;; "run" calls the imported host function "env"."yield" (() -> i32) three
;; times in a loop and returns the running sum. With a host that returns
;; host_calls*10 (10 + 20 + 30) the result is 60 after 3 suspend/resume cycles.
(module
  (type (;0;) (func (result i32)))
  (import "env" "yield" (func $yield (type 0)))
  (table 1 funcref)
  (elem (i32.const 0) $yield)
  (func $tail_host (export "tail_host") (type 0)
    return_call $yield)
  (func $tail_indirect_host (export "tail_indirect_host") (type 0)
    i32.const 0
    return_call_indirect (type 0))
  (func (export "call_tail_host") (type 0)
    call $tail_host
    i32.const 1
    i32.add)
  (func (export "call_tail_indirect_host") (type 0)
    call $tail_indirect_host
    i32.const 1
    i32.add)
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
