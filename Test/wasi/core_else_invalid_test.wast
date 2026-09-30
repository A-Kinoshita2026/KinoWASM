;; core_else_invalid_test.wast
;;
;; Regression tests for `else` placement validation (kw_parser.c, 2026-07-21).
;;
;; `else` (0x05) is only valid inside an `if` block. The parser accepted a
;; stray `else` in a block/loop/try, and a second `else` inside an if's else
;; arm, because the MODE_BLOCK_LOOP / MODE_BLOCK_ELSE_LOOP handlers never
;; checked the enclosing block kind. The core compiler then used the if-only
;; else_fixup (zero for a non-if frame, or an already-resolved index for the
;; second else) and wrote through it, corrupting the bytecode -> 0xC0000005.
;;
;; These modules cannot be written in wast text form (wabt rejects a lone
;; `else`), so they are supplied as raw binary via (module binary ...).

;; else inside a plain block
(assert_invalid
  (module binary
    "\00\61\73\6d\01\00\00\00\01\05\01\60\00\01\7f\03\02\01\00"
    "\0a\0c\01\0a\00\02\7f\41\01\05\41\02\0b\0b")
  "unexpected else")

;; else inside a loop
(assert_invalid
  (module binary
    "\00\61\73\6d\01\00\00\00\01\05\01\60\00\01\7f\03\02\01\00"
    "\0a\0c\01\0a\00\03\7f\41\01\05\41\02\0b\0b")
  "unexpected else")

;; a second else inside an if's else arm (arity matched so it would otherwise slip through)
(assert_invalid
  (module binary
    "\00\61\73\6d\01\00\00\00\01\05\01\60\00\01\7f\03\02\01\00"
    "\0a\0f\01\0d\00\41\01\04\7f\41\01\05\41\02\05\0b\0b")
  "unexpected else")
