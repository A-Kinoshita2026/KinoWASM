;; wasi_fs_write_test.wast
;;
;; Phase 3b で追加した書き込み系 WASI 関数の回帰テスト:
;;   path_open (CREATE+TRUNC+WRITE), fd_write, fd_pwrite,
;;   path_create_directory, path_remove_directory,
;;   path_unlink_file, path_rename
;;
;; testsuite_runner はこのテストの実行前に scratch directory を preopen する:
;;   wasi_add_preopen("Test/wasi_fs_write_scratch", ".")
;;
;; テストは「create+TRUNC で開く」のため、繰り返し実行しても scratch 内
;; ファイルが上書きされるだけで状態に依存しない。各テストは自分が使う
;; ファイルを毎回 path_unlink_file で削除して開始する想定。

(module
  (import "wasi_snapshot_preview1" "path_open"
    (func $path_open (param i32 i32 i32 i32 i32 i64 i64 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_close"
    (func $fd_close (param i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_write"
    (func $fd_write (param i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_pwrite"
    (func $fd_pwrite (param i32 i32 i32 i64 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_read"
    (func $fd_read (param i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_filestat_get"
    (func $fd_filestat_get (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "path_filestat_get"
    (func $path_filestat_get (param i32 i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "path_create_directory"
    (func $path_create_directory (param i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "path_remove_directory"
    (func $path_remove_directory (param i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "path_unlink_file"
    (func $path_unlink_file (param i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "path_rename"
    (func $path_rename (param i32 i32 i32 i32 i32 i32) (result i32)))

  (memory (export "mem") 1)

  ;; ファイル名定数
  (data (i32.const 100) "out.txt")          ;; 7 byte
  (data (i32.const 200) "renamed.txt")      ;; 11 byte
  (data (i32.const 300) "subdir")           ;; 6 byte
  ;; 書き込みデータ
  (data (i32.const 500) "ABCD")             ;; 4 byte
  (data (i32.const 600) "Z")                ;; 1 byte (overwrite first byte)

  ;; oflags / rights 定数
  ;;   __WASI_OFLAGS_CREAT = 1, __WASI_OFLAGS_TRUNC = 8
  ;;   __WASI_RIGHT_FD_READ = 1<<1 = 2, __WASI_RIGHT_FD_WRITE = 1<<6 = 64
  ;;   read+write = 66

  ;; ===========================================================
  ;; Helper: out.txt を CREATE+TRUNC で開いて fd を返す (失敗時 -1)
  ;; ===========================================================

  (func $open_out_create (result i32)
    (call $path_open
      (i32.const 3) (i32.const 0)
      (i32.const 100) (i32.const 7)               ;; "out.txt"
      (i32.const 9)                                ;; oflags = CREAT | TRUNC
      (i64.const 66) (i64.const 0)                 ;; rights = READ | WRITE
      (i32.const 0)
      (i32.const 1000))
    drop
    (i32.load (i32.const 1000)))

  (func $open_out_readonly (result i32)
    (call $path_open
      (i32.const 3) (i32.const 0)
      (i32.const 100) (i32.const 7)
      (i32.const 0)
      (i64.const 2) (i64.const 0)                  ;; rights = READ
      (i32.const 0)
      (i32.const 1004))
    drop
    (i32.load (i32.const 1004)))

  ;; ===========================================================
  ;; テスト: ファイル削除 (前ステップ後始末) → 失敗してもテスト本体は気にしない
  ;; ===========================================================

  (func (export "cleanup_out") (result i32)
    (call $path_unlink_file (i32.const 3) (i32.const 100) (i32.const 7))
    drop
    (i32.const 0))

  ;; ===========================================================
  ;; テスト 1: out.txt を CREATE+TRUNC で開いて 4 byte 書き込み
  ;; ===========================================================

  (func (export "write_create_4bytes") (result i32)
    (local $fd i32)
    (call $path_unlink_file (i32.const 3) (i32.const 100) (i32.const 7))
    drop
    (local.set $fd (call $open_out_create))
    ;; iovec @ 2000: { addr=500 ("ABCD"), len=4 }
    (i32.store (i32.const 2000) (i32.const 500))
    (i32.store (i32.const 2004) (i32.const 4))
    (call $fd_write (local.get $fd) (i32.const 2000) (i32.const 1) (i32.const 2010))
    drop
    (call $fd_close (local.get $fd))
    drop
    (i32.load (i32.const 2010)))   ;; 書き込んだ byte 数 = 4

  ;; テスト 2: 上で書いた out.txt を read-only で開いて 4 byte 読み戻し
  ;;   "ABCD" little-endian = 0x44434241
  (func (export "read_back_first4") (result i32)
    (local $fd i32)
    (local.set $fd (call $open_out_readonly))
    (i32.store (i32.const 3000) (i32.const 3100))
    (i32.store (i32.const 3004) (i32.const 4))
    (call $fd_read (local.get $fd) (i32.const 3000) (i32.const 1) (i32.const 3010))
    drop
    (call $fd_close (local.get $fd))
    drop
    (i32.load (i32.const 3100)))

  ;; テスト 3: path_filestat_get で out.txt の size = 4 を確認
  (func (export "filestat_size_4") (result i64)
    (call $path_filestat_get
      (i32.const 3) (i32.const 0)
      (i32.const 100) (i32.const 7)
      (i32.const 4000))
    drop
    (i64.load offset=32 (i32.const 4000)))

  ;; テスト 4: pwrite で先頭 1 byte を 'Z' に上書きして再読
  ;;   "ZBCD" LE = 0x4443425A
  (func (export "pwrite_overwrite_first") (result i32)
    (local $fd i32)
    ;; 既存の out.txt を read+write で開く (CREATE 無し、TRUNC 無し)
    (call $path_open
      (i32.const 3) (i32.const 0)
      (i32.const 100) (i32.const 7)
      (i32.const 0)
      (i64.const 66) (i64.const 0)
      (i32.const 0)
      (i32.const 5000))
    drop
    (local.set $fd (i32.load (i32.const 5000)))
    ;; iovec: addr=600 ("Z"), len=1
    (i32.store (i32.const 5100) (i32.const 600))
    (i32.store (i32.const 5104) (i32.const 1))
    (call $fd_pwrite
      (local.get $fd)
      (i32.const 5100) (i32.const 1)
      (i64.const 0)              ;; offset
      (i32.const 5110))
    drop
    (call $fd_close (local.get $fd))
    drop
    ;; 読み戻す
    (local.set $fd (call $open_out_readonly))
    (i32.store (i32.const 5200) (i32.const 5300))
    (i32.store (i32.const 5204) (i32.const 4))
    (call $fd_read (local.get $fd) (i32.const 5200) (i32.const 1) (i32.const 5210))
    drop
    (call $fd_close (local.get $fd))
    drop
    (i32.load (i32.const 5300)))

  ;; ===========================================================
  ;; テスト 5: rename で out.txt → renamed.txt
  ;; ===========================================================

  (func (export "rename_file") (result i32)
    ;; renamed.txt が残っていれば消しておく
    (call $path_unlink_file (i32.const 3) (i32.const 200) (i32.const 11))
    drop
    (call $path_rename
      (i32.const 3) (i32.const 100) (i32.const 7)    ;; "out.txt"
      (i32.const 3) (i32.const 200) (i32.const 11))) ;; "renamed.txt"

  ;; rename 後、元 path は無いので filestat は EBADF
  (func (export "rename_old_gone") (result i32)
    (call $path_filestat_get
      (i32.const 3) (i32.const 0)
      (i32.const 100) (i32.const 7)
      (i32.const 6000)))

  ;; rename 後、新 path は存在
  (func (export "rename_new_exists") (result i32)
    (call $path_filestat_get
      (i32.const 3) (i32.const 0)
      (i32.const 200) (i32.const 11)
      (i32.const 6000)))

  ;; ===========================================================
  ;; テスト 6: mkdir / rmdir
  ;; ===========================================================

  (func (export "mkdir_subdir") (result i32)
    ;; 既に存在するなら一旦消す
    (call $path_remove_directory (i32.const 3) (i32.const 300) (i32.const 6))
    drop
    (call $path_create_directory (i32.const 3) (i32.const 300) (i32.const 6)))

  (func (export "rmdir_subdir") (result i32)
    (call $path_remove_directory (i32.const 3) (i32.const 300) (i32.const 6)))

  ;; ===========================================================
  ;; 終了処理: renamed.txt も消しておく (再実行時に rename テストが
  ;; 残ファイルにぶつからないように)
  ;; ===========================================================

  (func (export "final_cleanup") (result i32)
    (call $path_unlink_file (i32.const 3) (i32.const 200) (i32.const 11)))

  ;; path traversal 拒否は引き続き有効か (writeパスで)
  (data (i32.const 700) "../bad.txt")

  (func (export "open_traversal_blocked_w") (result i32)
    (call $path_open
      (i32.const 3) (i32.const 0)
      (i32.const 700) (i32.const 10)
      (i32.const 9)                                  ;; CREAT+TRUNC
      (i64.const 66) (i64.const 0)
      (i32.const 0)
      (i32.const 7000)))
)

;; ============================================================
;; assert_return
;; ============================================================

;; テスト 1: 4 byte 書き込みが返る
(assert_return (invoke "write_create_4bytes")  (i32.const 4))
;; テスト 2: 読み戻しで "ABCD" LE = 0x44434241
(assert_return (invoke "read_back_first4")     (i32.const 0x44434241))
;; テスト 3: ファイルサイズ = 4
(assert_return (invoke "filestat_size_4")      (i64.const 4))
;; テスト 4: pwrite 後の読み戻し "ZBCD" LE = 0x44434245A は 0x4443425A
(assert_return (invoke "pwrite_overwrite_first") (i32.const 0x4443425A))
;; テスト 5: rename 成功 / 元 path 消失 / 新 path 存在
(assert_return (invoke "rename_file")        (i32.const 0))
(assert_return (invoke "rename_old_gone")    (i32.const 8))   ;; EBADF
(assert_return (invoke "rename_new_exists")  (i32.const 0))
;; テスト 6: mkdir / rmdir
(assert_return (invoke "mkdir_subdir")  (i32.const 0))
(assert_return (invoke "rmdir_subdir")  (i32.const 0))
;; 後始末
(assert_return (invoke "final_cleanup")  (i32.const 0))
;; path traversal は CREATE+WRITE モードでも拒否される
(assert_return (invoke "open_traversal_blocked_w")  (i32.const 28))   ;; EINVAL
