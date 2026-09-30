;; wasi_args_test.wast
;;
;; argv / environ が実際に WASM 側に伝播するかを検証する。
;; testsuite_runner はこの test ファイル群の実行前に既知の argv/environ をセットし、
;; 終了後に空に戻す。期待値はその既知値に対応している。
;;
;; 想定する set 値:
;;   argv    = {"runner_test", "alpha"}                      (2 件、合計 18 byte)
;;   environ = {"FOO=bar", "BAZ=qux"}                        (2 件、合計 16 byte)

(module
  (import "wasi_snapshot_preview1" "args_sizes_get"
    (func $args_sizes_get (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "args_get"
    (func $args_get (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "environ_sizes_get"
    (func $environ_sizes_get (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "environ_get"
    (func $environ_get (param i32 i32) (result i32)))

  (memory (export "mem") 1)

  ;; ===========================================================
  ;; argv: count = 2, buf size = strlen("runner_test")+1 + strlen("alpha")+1 = 12 + 6 = 18
  ;; ===========================================================

  (func (export "args_sizes_count") (result i32)
    (call $args_sizes_get (i32.const 0) (i32.const 4))
    drop
    (i32.load (i32.const 0)))

  (func (export "args_sizes_buf_size") (result i32)
    (call $args_sizes_get (i32.const 0) (i32.const 4))
    drop
    (i32.load (i32.const 4)))

  ;; args_get で argv ポインタ配列 (offset 0) と文字列バッファ (offset 100) に展開、
  ;; 第 0 引数の先頭 4 byte ('r','u','n','n') と一致するか検証する。
  ;; 期待値: 0x6E_6E_75_72 (リトルエンディアンで "runn" -> 0x72='r', 0x75='u', 0x6E='n', 0x6E='n')
  (func (export "args_get_first_arg_first4") (result i32)
    (call $args_get (i32.const 0) (i32.const 100))
    drop
    ;; argv[0] は memory[0..3] にあるポインタ (= 100 を期待)
    (i32.load (i32.load (i32.const 0))))

  ;; argv[0] のポインタ = 100 (buf_addr) であること
  (func (export "args_get_first_arg_ptr") (result i32)
    (call $args_get (i32.const 0) (i32.const 100))
    drop
    (i32.load (i32.const 0)))

  ;; argv[1] のポインタ = 100 + 12 = 112 ("runner_test\0" の次)
  (func (export "args_get_second_arg_ptr") (result i32)
    (call $args_get (i32.const 0) (i32.const 100))
    drop
    (i32.load (i32.const 4)))

  ;; argv[1] の先頭 4 byte ("alph" = 0x68706C61)
  (func (export "args_get_second_arg_first4") (result i32)
    (call $args_get (i32.const 0) (i32.const 100))
    drop
    (i32.load (i32.load (i32.const 4))))

  ;; ===========================================================
  ;; environ: count = 2, buf size = strlen("FOO=bar")+1 + strlen("BAZ=qux")+1 = 8 + 8 = 16
  ;; ===========================================================

  (func (export "environ_sizes_count") (result i32)
    (call $environ_sizes_get (i32.const 0) (i32.const 4))
    drop
    (i32.load (i32.const 0)))

  (func (export "environ_sizes_buf_size") (result i32)
    (call $environ_sizes_get (i32.const 0) (i32.const 4))
    drop
    (i32.load (i32.const 4)))

  ;; environ_get でポインタ配列 (offset 0) + buf (offset 200)
  ;; 期待 environ[0] の先頭 4 byte = "FOO=" = 0x3D_4F_4F_46 (LE: 'F','O','O','=')
  (func (export "environ_get_first_first4") (result i32)
    (call $environ_get (i32.const 0) (i32.const 200))
    drop
    (i32.load (i32.load (i32.const 0))))

  (func (export "environ_get_second_ptr") (result i32)
    (call $environ_get (i32.const 0) (i32.const 200))
    drop
    (i32.load (i32.const 4)))

  ;; environ[1] の先頭 4 byte = "BAZ=" = 0x3D_5A_41_42
  (func (export "environ_get_second_first4") (result i32)
    (call $environ_get (i32.const 0) (i32.const 200))
    drop
    (i32.load (i32.load (i32.const 4))))
)

;; ============================================================
;; assert_return
;; ============================================================

;; argv counts
(assert_return (invoke "args_sizes_count")     (i32.const 2))
(assert_return (invoke "args_sizes_buf_size")  (i32.const 18))

;; argv content: "runn" little-endian = 0x6E_6E_75_72
(assert_return (invoke "args_get_first_arg_first4")  (i32.const 0x6E6E7572))
(assert_return (invoke "args_get_first_arg_ptr")     (i32.const 100))
(assert_return (invoke "args_get_second_arg_ptr")    (i32.const 112))
;; "alph" little-endian = 0x68_70_6C_61
(assert_return (invoke "args_get_second_arg_first4") (i32.const 0x68706C61))

;; environ counts
(assert_return (invoke "environ_sizes_count")     (i32.const 2))
(assert_return (invoke "environ_sizes_buf_size")  (i32.const 16))

;; "FOO=" LE = 0x3D_4F_4F_46
(assert_return (invoke "environ_get_first_first4")  (i32.const 0x3D4F4F46))
(assert_return (invoke "environ_get_second_ptr")    (i32.const 208))
;; "BAZ=" LE = 0x3D_5A_41_42
(assert_return (invoke "environ_get_second_first4") (i32.const 0x3D5A4142))
