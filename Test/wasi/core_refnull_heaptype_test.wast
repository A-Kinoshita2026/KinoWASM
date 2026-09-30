;; core_refnull_heaptype_test.wast
;;
;; Regression: ref.null heaptype width mismatch (kw_parser_ops_base.inc OP_REF_NULL,
;; 2026-07-10).
;;
;; The parser read the ref.null heaptype as a single byte (read_u8) while the core
;; compiler reads it as a multi-byte SLEB (rds64, kw_core_compile.c case 0xd0). A
;; heaptype with the continuation bit set (0x80...) or a typeidx encoding therefore
;; desynced the parser/core instruction streams: the core consumed the following
;; opcode/operand bytes as part of the heaptype and misread the rest of the body
;; (observed: a following drop was swallowed, or the function end 0x0b was eaten).
;; The fix limits ref.null to funcref/externref/exnref and rejects anything else as
;; malformed, so parser and core always consume the same single byte.

;; valid: ref.null func / extern parse and instantiate fine (unchanged behavior)
(module
  (func (export "f") (result funcref) (ref.null func))
  (func (export "e") (result externref) (ref.null extern)))

;; malformed: heaptype 0x80 (continuation bit set). core rds64 would consume the
;; following drop (0x1a). body = ref.null 0x80 ; drop ; end
(assert_malformed
  (module binary
    "\00asm\01\00\00\00"
    "\01\04\01\60\00\00"          ;; type: () -> ()
    "\03\02\01\00"                ;; func: type 0
    "\0a\07\01\05\00\d0\80\1a\0b" ;; code: ref.null 0x80 ; drop ; end
  )
  "malformed reference type")

;; malformed: heaptype 0x00 (typeidx encoding; typed ref.null is unsupported here).
;; Same byte width as the fix, but 0x00 is not a valid abstract reftype.
(assert_malformed
  (module binary
    "\00asm\01\00\00\00"
    "\01\04\01\60\00\00"
    "\03\02\01\00"
    "\0a\07\01\05\00\d0\00\1a\0b" ;; code: ref.null 0x00 ; drop ; end
  )
  "malformed reference type")

;; malformed: heaptype 0x80 as the last byte before end (result funcref). core rds64
;; would consume the 0x0b end byte as a continuation. body = ref.null 0x80 ; end
(assert_malformed
  (module binary
    "\00asm\01\00\00\00"
    "\01\05\01\60\00\01\70"       ;; type: () -> funcref
    "\03\02\01\00"
    "\0a\06\01\04\00\d0\80\0b"    ;; code: ref.null 0x80 ; end
  )
  "malformed reference type")
