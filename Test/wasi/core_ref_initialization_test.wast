;; Reference defaults on every function-entry path; numeric locals stay zero.
(module $refs
  (type $check_type (func (param i32) (result i32)))
  (table 1 funcref)
  (elem (i32.const 0) $check)
  (func $check (export "check") (type $check_type)
    (local funcref externref exnref i32 i64 f32 f64)
    local.get 0 i32.const 37 i32.eq
    local.get 1 ref.is_null i32.and
    local.get 2 ref.is_null i32.and
    local.get 3 ref.is_null i32.and
    local.get 4 i32.eqz i32.and
    local.get 5 i64.eqz i32.and
    local.get 6 f32.const 0 f32.eq i32.and
    local.get 7 f64.const 0 f64.eq i32.and)
  (func (export "direct") (result i32) i32.const 37 call $check)
  (func (export "indirect") (result i32)
    i32.const 37 i32.const 0 call_indirect (type $check_type))
  (func (export "tail") (result i32) i32.const 37 return_call $check)
  (func (export "tail_indirect") (result i32)
    i32.const 37 i32.const 0 return_call_indirect (type $check_type))
  (func $param (param funcref) (result i32) (local funcref)
    local.get 0 ref.is_null i32.eqz
    local.get 1 ref.is_null i32.and)
  (func (export "parameter") (result i32) ref.func $check call $param)
  (func (export "null_func") (result i32) (local funcref)
    local.get 0 ref.is_null)
  (func (export "null_extern") (result i32) (local externref)
    local.get 0 ref.is_null)
  (func (export "null_exn") (result i32) (local exnref)
    local.get 0 ref.is_null)
  (func (export "explicit_exn") (result i32) ref.null exn ref.is_null)
  (global $null_exn exnref (ref.null exn))
  (func $throw_null (export "throw_null") ref.null exn throw_ref)
  (func (export "throw_local") (local exnref) local.get 0 throw_ref)
  (func (export "throw_global") global.get $null_exn throw_ref)
  (func (export "catch_null") (result i32)
    block $caught
      try_table (catch_all $caught) ref.null exn throw_ref end
      i32.const 0 return
    end
    i32.const 99)
  (func (export "catch_local") (result i32) (local exnref)
    block $caught
      try_table (catch_all $caught) local.get 0 throw_ref end
      i32.const 0 return
    end
    i32.const 99)
  (func (export "catch_call") (result i32)
    block $caught
      try_table (catch_all $caught) call $throw_null end
      i32.const 0 return
    end
    i32.const 99)
  (tag $e (param i32))
  ;; A real exception reference must still survive local.set and throw_ref.
  (func (export "rethrow_ref") (result i32) (local exnref)
    block $caught (result i32)
      try_table (catch $e $caught)
        block $saved (result exnref)
          try_table (catch_all_ref $saved) i32.const 73 throw $e end
          unreachable
        end
        local.set 0
        local.get 0 throw_ref
      end
      i32.const 99
    end))

(assert_return (invoke $refs "check" (i32.const 37)) (i32.const 1))
(assert_return (invoke $refs "direct") (i32.const 1))
(assert_return (invoke $refs "indirect") (i32.const 1))
(assert_return (invoke $refs "tail") (i32.const 1))
(assert_return (invoke $refs "tail_indirect") (i32.const 1))
(assert_return (invoke $refs "parameter") (i32.const 1))
(assert_return (invoke $refs "null_func") (i32.const 1))
(assert_return (invoke $refs "null_extern") (i32.const 1))
(assert_return (invoke $refs "null_exn") (i32.const 1))
(assert_return (invoke $refs "explicit_exn") (i32.const 1))
(assert_trap (invoke $refs "throw_null") "null exception reference")
(assert_trap (invoke $refs "throw_local") "null exception reference")
(assert_trap (invoke $refs "throw_global") "null exception reference")
(assert_trap (invoke $refs "catch_null") "null exception reference")
(assert_trap (invoke $refs "catch_local") "null exception reference")
(assert_trap (invoke $refs "catch_call") "null exception reference")
(assert_return (invoke $refs "rethrow_ref") (i32.const 73))

(register "refs" $refs)
(module
  (import "refs" "check" (func $check (param i32) (result i32)))
  (table 1 funcref)
  (elem (i32.const 0) $check)
  (type $check_type (func (param i32) (result i32)))
  (func (export "cross") (result i32) i32.const 37 call $check)
  (func (export "cross_tail") (result i32) i32.const 37 return_call $check)
  (func (export "cross_indirect") (result i32)
    i32.const 37 i32.const 0 call_indirect (type $check_type))
  (func (export "cross_tail_indirect") (result i32)
    i32.const 37 i32.const 0 return_call_indirect (type $check_type)))
(assert_return (invoke "cross") (i32.const 1))
(assert_return (invoke "cross_tail") (i32.const 1))
(assert_return (invoke "cross_indirect") (i32.const 1))
(assert_return (invoke "cross_tail_indirect") (i32.const 1))
