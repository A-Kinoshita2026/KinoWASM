;; ホスト関数 (extra_func_table) reexport の回帰 (2026-07-22)。
;; resolve_imports が extra_func_type 台帳を毎ロード memset で全クリアしていたため、
;; ホスト関数を import して reexport したモジュール経由の import 解決が、後段の
;; 台帳 NULL 検査で常に ERR_INCOMPATIBLE_IMPORT_TYPE になった。台帳を store 寿命の
;; 累積 (rebuild_extra_func_type と同セマンティクス) に修正し、既存記録を保持する。

(module $reexporter
  (import "spectest" "print_i32" (func $p (param i32)))
  (export "print_i32_re" (func $p))
)
(register "reexporter" $reexporter)

;; reexport 経由の import が解決・呼出できる
(module
  (import "reexporter" "print_i32_re" (func $p2 (param i32)))
  (func (export "go") (call $p2 (i32.const 42)))
)
(assert_return (invoke "go"))

;; reexport 経由でも宣言型は台帳の初回宣言と照合される (不一致は unlinkable)
(assert_unlinkable
  (module (import "reexporter" "print_i32_re" (func (param f64))))
  "incompatible import type")

;; 直接 host import も台帳が累積するため初回宣言 (param i32) と照合される
(assert_unlinkable
  (module (import "spectest" "print_i32" (func (param i64 i64))))
  "incompatible import type")
