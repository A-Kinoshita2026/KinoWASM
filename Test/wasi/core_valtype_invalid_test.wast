;; core_valtype_invalid_test.wast
;;
;; Regression tests for valtype byte validation (kw_parser.c, 2026-07-22).
;;
;; The parser read type bytes in functype (param/result), globaltype, local
;; declarations and select_t without checking they are a valid valtype
;; (only the SIMD gate rejected 0x7B). In particular 0x00 collides with the
;; type_stack_pop "matches anything" marker, so an invalid module could push
;; a wildcard type onto the type stack and bypass all subsequent type checks.
;; All valtype-reading sites now share the kw_parser_is_valtype accept list.
;;
;; These modules cannot be written in wast text form, so they are supplied
;; as raw binary via (module binary ...).

;; functype param 0x00
(assert_invalid
  (module binary
    "\00\61\73\6d\01\00\00\00"
    "\01\05\01\60\01\00\00")
  "malformed value type")

;; functype result 0x25
(assert_invalid
  (module binary
    "\00\61\73\6d\01\00\00\00"
    "\01\05\01\60\00\01\25")
  "malformed value type")

;; globaltype 0x00 with i32.const init
(assert_invalid
  (module binary
    "\00\61\73\6d\01\00\00\00"
    "\06\06\01\00\00\41\00\0b")
  "malformed value type")

;; local declaration 0x00 + local.get (wildcard local would defeat type checks)
(assert_invalid
  (module binary
    "\00\61\73\6d\01\00\00\00"
    "\01\04\01\60\00\00\03\02\01\00"
    "\0a\09\01\07\01\01\00\20\00\1a\0b")
  "malformed value type")

;; select_t with type 0x00 (would become an unchecked select)
(assert_invalid
  (module binary
    "\00\61\73\6d\01\00\00\00"
    "\01\04\01\60\00\00\03\02\01\00"
    "\0a\0e\01\0c\00\41\01\41\02\41\00\1c\01\00\1a\0b")
  "malformed value type")

;; select_t with type 0x25 after unreachable (polymorphic pops accept anything)
(assert_invalid
  (module binary
    "\00\61\73\6d\01\00\00\00"
    "\01\04\01\60\00\00\03\02\01\00"
    "\0a\08\01\06\00\00\1c\01\25\0b")
  "malformed value type")
