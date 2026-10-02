(module
  (import "env" "next_frame" (func $next_frame))
  (import "env" "report_step" (func $report_step (param i32)))
  ;; This nested call keeps a local alive across a host-triggered suspension.
  (func $step (param $value i32) (result i32)
    local.get $value
    call $report_step
    call $next_frame
    local.get $value)
  (func (export "run") (result i32)
    i32.const 10
    call $step
    i32.const 20
    call $step
    i32.add
    i32.const 30
    call $step
    i32.add))
