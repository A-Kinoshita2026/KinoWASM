;; core_import_indextype_test.wast
;;
;; Regression: import matching ignored the memory64/table64 index type
;; (kinowasm.c match_limits, 2026-07-10).
;;
;; match_limits only compared min/max/has_max and did not check is_64 (or shared),
;; so a memory64/table64 import could be satisfied by a memory32/table32 instance
;; (and vice versa). The importer's code is validated for i64 addresses but the
;; core compiles the real instance as memory32, desyncing the address type. The fix
;; requires is_64 and shared to match exactly.
;;
;; wast2json feature flag: --enable-memory64 (memory/table i64 syntax).

(module $A32
  (memory (export "mem") 1)
  (table (export "tab") 1 funcref))
(register "A32" $A32)

(module $A64
  (memory (export "mem64") i64 1))
(register "A64" $A64)

;; positive: same index type links fine
(module (import "A32" "mem" (memory 1)))
(module (import "A32" "tab" (table 1 funcref)))
(module (import "A64" "mem64" (memory i64 1)))

;; memory64 import cannot be satisfied by a memory32 export
(assert_unlinkable
  (module (import "A32" "mem" (memory i64 1)))
  "incompatible import type")

;; memory32 import cannot be satisfied by a memory64 export
(assert_unlinkable
  (module (import "A64" "mem64" (memory 1)))
  "incompatible import type")

;; table64 import cannot be satisfied by a table32 export
(assert_unlinkable
  (module (import "A32" "tab" (table i64 1 funcref)))
  "incompatible import type")
