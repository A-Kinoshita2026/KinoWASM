;; Complete exception payloads through local, call, rethrow and exnref paths.
(module
  (tag $e (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32))
  (func $raise (local i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32) i32.const 1 i32.const 2 i32.const 3 i32.const 4 i32.const 5 i32.const 6 i32.const 7 i32.const 8 i32.const 9 i32.const 10 i32.const 11 i32.const 12 i32.const 13 i32.const 14 i32.const 15 i32.const 16 throw $e)
  (func $rethrow
    try call $raise catch_all rethrow 0 end)
  (func $throw_ref (param exnref) local.get 0 throw_ref)
  (func (export "call") (result i32) (local $last i32)
    try (result i32) call $raise unreachable catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end)
  (func (export "local") (result i32) (local $last i32)
    try (result i32) i32.const 1 i32.const 2 i32.const 3 i32.const 4 i32.const 5 i32.const 6 i32.const 7 i32.const 8 i32.const 9 i32.const 10 i32.const 11 i32.const 12 i32.const 13 i32.const 14 i32.const 15 i32.const 16 throw $e catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end)
  (func (export "rethrow") (result i32) (local $last i32)
    try (result i32) call $rethrow unreachable catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end)
  (func (export "rethrow_local") (result i32) (local $last i32)
    try (result i32)
      try call $raise catch_all rethrow 0 end
      unreachable
    catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end)
  (func (export "try_table") (result i32) (local $last i32)
    block $caught (result i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32)
      try_table (catch $e $caught) call $raise end
      unreachable
    end
    local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last)
  (func (export "exnref") (result i32) (local $last i32)
    try (result i32)
      block $ref (result exnref)
        try_table (catch_all_ref $ref) call $raise end
        unreachable
      end
      call $throw_ref unreachable
    catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end)
  (func (export "exnref_local") (result i32) (local $last i32)
    try (result i32)
      block $ref (result exnref)
        try_table (catch_all_ref $ref) call $raise end
        unreachable
      end
      throw_ref
    catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end))

(assert_return (invoke "call") (i32.const 16))
(assert_return (invoke "local") (i32.const 16))
(assert_return (invoke "rethrow") (i32.const 16))
(assert_return (invoke "rethrow_local") (i32.const 16))
(assert_return (invoke "try_table") (i32.const 16))
(assert_return (invoke "exnref") (i32.const 16))
(assert_return (invoke "exnref_local") (i32.const 16))
(module
  (tag $e (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32))
  (func $raise (local i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32) i32.const 1 i32.const 2 i32.const 3 i32.const 4 i32.const 5 i32.const 6 i32.const 7 i32.const 8 i32.const 9 i32.const 10 i32.const 11 i32.const 12 i32.const 13 i32.const 14 i32.const 15 i32.const 16 i32.const 17 throw $e)
  (func $rethrow
    try call $raise catch_all rethrow 0 end)
  (func $throw_ref (param exnref) local.get 0 throw_ref)
  (func (export "call") (result i32) (local $last i32)
    try (result i32) call $raise unreachable catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end)
  (func (export "local") (result i32) (local $last i32)
    try (result i32) i32.const 1 i32.const 2 i32.const 3 i32.const 4 i32.const 5 i32.const 6 i32.const 7 i32.const 8 i32.const 9 i32.const 10 i32.const 11 i32.const 12 i32.const 13 i32.const 14 i32.const 15 i32.const 16 i32.const 17 throw $e catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end)
  (func (export "rethrow") (result i32) (local $last i32)
    try (result i32) call $rethrow unreachable catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end)
  (func (export "rethrow_local") (result i32) (local $last i32)
    try (result i32)
      try call $raise catch_all rethrow 0 end
      unreachable
    catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end)
  (func (export "try_table") (result i32) (local $last i32)
    block $caught (result i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32)
      try_table (catch $e $caught) call $raise end
      unreachable
    end
    local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last)
  (func (export "exnref") (result i32) (local $last i32)
    try (result i32)
      block $ref (result exnref)
        try_table (catch_all_ref $ref) call $raise end
        unreachable
      end
      call $throw_ref unreachable
    catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end)
  (func (export "exnref_local") (result i32) (local $last i32)
    try (result i32)
      block $ref (result exnref)
        try_table (catch_all_ref $ref) call $raise end
        unreachable
      end
      throw_ref
    catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end))

(assert_return (invoke "call") (i32.const 17))
(assert_return (invoke "local") (i32.const 17))
(assert_return (invoke "rethrow") (i32.const 17))
(assert_return (invoke "rethrow_local") (i32.const 17))
(assert_return (invoke "try_table") (i32.const 17))
(assert_return (invoke "exnref") (i32.const 17))
(assert_return (invoke "exnref_local") (i32.const 17))
(module
  (tag $e (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32))
  (func $raise (local i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32) i32.const 1 i32.const 2 i32.const 3 i32.const 4 i32.const 5 i32.const 6 i32.const 7 i32.const 8 i32.const 9 i32.const 10 i32.const 11 i32.const 12 i32.const 13 i32.const 14 i32.const 15 i32.const 16 i32.const 17 i32.const 18 i32.const 19 i32.const 20 throw $e)
  (func $rethrow
    try call $raise catch_all rethrow 0 end)
  (func $throw_ref (param exnref) local.get 0 throw_ref)
  (func (export "call") (result i32) (local $last i32)
    try (result i32) call $raise unreachable catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end)
  (func (export "local") (result i32) (local $last i32)
    try (result i32) i32.const 1 i32.const 2 i32.const 3 i32.const 4 i32.const 5 i32.const 6 i32.const 7 i32.const 8 i32.const 9 i32.const 10 i32.const 11 i32.const 12 i32.const 13 i32.const 14 i32.const 15 i32.const 16 i32.const 17 i32.const 18 i32.const 19 i32.const 20 throw $e catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end)
  (func (export "rethrow") (result i32) (local $last i32)
    try (result i32) call $rethrow unreachable catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end)
  (func (export "rethrow_local") (result i32) (local $last i32)
    try (result i32)
      try call $raise catch_all rethrow 0 end
      unreachable
    catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end)
  (func (export "try_table") (result i32) (local $last i32)
    block $caught (result i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32)
      try_table (catch $e $caught) call $raise end
      unreachable
    end
    local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last)
  (func (export "exnref") (result i32) (local $last i32)
    try (result i32)
      block $ref (result exnref)
        try_table (catch_all_ref $ref) call $raise end
        unreachable
      end
      call $throw_ref unreachable
    catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end)
  (func (export "exnref_local") (result i32) (local $last i32)
    try (result i32)
      block $ref (result exnref)
        try_table (catch_all_ref $ref) call $raise end
        unreachable
      end
      throw_ref
    catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end))

(assert_return (invoke "call") (i32.const 20))
(assert_return (invoke "local") (i32.const 20))
(assert_return (invoke "rethrow") (i32.const 20))
(assert_return (invoke "rethrow_local") (i32.const 20))
(assert_return (invoke "try_table") (i32.const 20))
(assert_return (invoke "exnref") (i32.const 20))
(assert_return (invoke "exnref_local") (i32.const 20))
(module
  (tag $e (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32))
  (func $raise (local i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32) i32.const 1 i32.const 2 i32.const 3 i32.const 4 i32.const 5 i32.const 6 i32.const 7 i32.const 8 i32.const 9 i32.const 10 i32.const 11 i32.const 12 i32.const 13 i32.const 14 i32.const 15 i32.const 16 i32.const 17 i32.const 18 i32.const 19 i32.const 20 i32.const 21 i32.const 22 i32.const 23 i32.const 24 i32.const 25 i32.const 26 i32.const 27 i32.const 28 i32.const 29 i32.const 30 i32.const 31 i32.const 32 i32.const 33 i32.const 34 i32.const 35 i32.const 36 i32.const 37 i32.const 38 i32.const 39 i32.const 40 i32.const 41 i32.const 42 i32.const 43 i32.const 44 i32.const 45 i32.const 46 i32.const 47 i32.const 48 i32.const 49 i32.const 50 i32.const 51 i32.const 52 i32.const 53 i32.const 54 i32.const 55 i32.const 56 i32.const 57 i32.const 58 i32.const 59 i32.const 60 i32.const 61 i32.const 62 i32.const 63 i32.const 64 i32.const 65 i32.const 66 i32.const 67 i32.const 68 i32.const 69 i32.const 70 i32.const 71 i32.const 72 i32.const 73 i32.const 74 i32.const 75 i32.const 76 i32.const 77 i32.const 78 i32.const 79 i32.const 80 i32.const 81 i32.const 82 i32.const 83 i32.const 84 i32.const 85 i32.const 86 i32.const 87 i32.const 88 i32.const 89 i32.const 90 i32.const 91 i32.const 92 i32.const 93 i32.const 94 i32.const 95 i32.const 96 i32.const 97 i32.const 98 i32.const 99 i32.const 100 i32.const 101 i32.const 102 i32.const 103 i32.const 104 i32.const 105 i32.const 106 i32.const 107 i32.const 108 i32.const 109 i32.const 110 i32.const 111 i32.const 112 i32.const 113 i32.const 114 i32.const 115 i32.const 116 i32.const 117 i32.const 118 i32.const 119 i32.const 120 i32.const 121 i32.const 122 i32.const 123 i32.const 124 i32.const 125 i32.const 126 i32.const 127 i32.const 128 i32.const 129 throw $e)
  (func $rethrow
    try call $raise catch_all rethrow 0 end)
  (func $throw_ref (param exnref) local.get 0 throw_ref)
  (func (export "call") (result i32) (local $last i32)
    try (result i32) call $raise unreachable catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end)
  (func (export "local") (result i32) (local $last i32)
    try (result i32) i32.const 1 i32.const 2 i32.const 3 i32.const 4 i32.const 5 i32.const 6 i32.const 7 i32.const 8 i32.const 9 i32.const 10 i32.const 11 i32.const 12 i32.const 13 i32.const 14 i32.const 15 i32.const 16 i32.const 17 i32.const 18 i32.const 19 i32.const 20 i32.const 21 i32.const 22 i32.const 23 i32.const 24 i32.const 25 i32.const 26 i32.const 27 i32.const 28 i32.const 29 i32.const 30 i32.const 31 i32.const 32 i32.const 33 i32.const 34 i32.const 35 i32.const 36 i32.const 37 i32.const 38 i32.const 39 i32.const 40 i32.const 41 i32.const 42 i32.const 43 i32.const 44 i32.const 45 i32.const 46 i32.const 47 i32.const 48 i32.const 49 i32.const 50 i32.const 51 i32.const 52 i32.const 53 i32.const 54 i32.const 55 i32.const 56 i32.const 57 i32.const 58 i32.const 59 i32.const 60 i32.const 61 i32.const 62 i32.const 63 i32.const 64 i32.const 65 i32.const 66 i32.const 67 i32.const 68 i32.const 69 i32.const 70 i32.const 71 i32.const 72 i32.const 73 i32.const 74 i32.const 75 i32.const 76 i32.const 77 i32.const 78 i32.const 79 i32.const 80 i32.const 81 i32.const 82 i32.const 83 i32.const 84 i32.const 85 i32.const 86 i32.const 87 i32.const 88 i32.const 89 i32.const 90 i32.const 91 i32.const 92 i32.const 93 i32.const 94 i32.const 95 i32.const 96 i32.const 97 i32.const 98 i32.const 99 i32.const 100 i32.const 101 i32.const 102 i32.const 103 i32.const 104 i32.const 105 i32.const 106 i32.const 107 i32.const 108 i32.const 109 i32.const 110 i32.const 111 i32.const 112 i32.const 113 i32.const 114 i32.const 115 i32.const 116 i32.const 117 i32.const 118 i32.const 119 i32.const 120 i32.const 121 i32.const 122 i32.const 123 i32.const 124 i32.const 125 i32.const 126 i32.const 127 i32.const 128 i32.const 129 throw $e catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end)
  (func (export "rethrow") (result i32) (local $last i32)
    try (result i32) call $rethrow unreachable catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end)
  (func (export "rethrow_local") (result i32) (local $last i32)
    try (result i32)
      try call $raise catch_all rethrow 0 end
      unreachable
    catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end)
  (func (export "try_table") (result i32) (local $last i32)
    block $caught (result i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32)
      try_table (catch $e $caught) call $raise end
      unreachable
    end
    local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last)
  (func (export "exnref") (result i32) (local $last i32)
    try (result i32)
      block $ref (result exnref)
        try_table (catch_all_ref $ref) call $raise end
        unreachable
      end
      call $throw_ref unreachable
    catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end)
  (func (export "exnref_local") (result i32) (local $last i32)
    try (result i32)
      block $ref (result exnref)
        try_table (catch_all_ref $ref) call $raise end
        unreachable
      end
      throw_ref
    catch $e local.set $last drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop drop local.get $last end))

(assert_return (invoke "call") (i32.const 129))
(assert_return (invoke "local") (i32.const 129))
(assert_return (invoke "rethrow") (i32.const 129))
(assert_return (invoke "rethrow_local") (i32.const 129))
(assert_return (invoke "try_table") (i32.const 129))
(assert_return (invoke "exnref") (i32.const 129))
(assert_return (invoke "exnref_local") (i32.const 129))
