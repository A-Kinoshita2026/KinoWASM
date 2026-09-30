;; wasi_fs_test.wast
;;
;; WASI Phase 3a (read-only file access) の回帰テスト。
;; testsuite_runner はこのテストの実行前に preopen を 1 つ登録する:
;;   wasi_add_preopen("Test/wasi_fs_data", ".")  → fd 3
;;
;; テストデータ:
;;   hello.txt  : "Hello, WASI!" (12 byte)
;;   digits.bin : "01234567"     (8 byte)

(module
  (import "wasi_snapshot_preview1" "fd_prestat_get"
    (func $fd_prestat_get (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_prestat_dir_name"
    (func $fd_prestat_dir_name (param i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "path_open"
    (func $path_open (param i32 i32 i32 i32 i32 i64 i64 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_close"
    (func $fd_close (param i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_read"
    (func $fd_read (param i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_seek"
    (func $fd_seek (param i32 i64 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_tell"
    (func $fd_tell (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_pread"
    (func $fd_pread (param i32 i32 i32 i64 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_filestat_get"
    (func $fd_filestat_get (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "path_filestat_get"
    (func $path_filestat_get (param i32 i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_fdstat_get"
    (func $fd_fdstat_get (param i32 i32) (result i32)))

  (memory (export "mem") 1)

  ;; 文字列定数 (memory 内の固定アドレス)
  (data (i32.const 100) "hello.txt")    ;; 9 byte
  (data (i32.const 200) "../escape")    ;; 9 byte (path traversal 試行)
  (data (i32.const 300) "digits.bin")   ;; 10 byte
  (data (i32.const 400) "missing.txt")  ;; 11 byte (存在しないファイル)

  ;; ============================================================
  ;; preopen 列挙
  ;; ============================================================

  (func (export "prestat_get_fd3_ok") (result i32)
    (call $fd_prestat_get (i32.const 3) (i32.const 0)))

  ;; wasi_name = "." → 長さ 1
  (func (export "prestat_get_namelen") (result i32)
    (call $fd_prestat_get (i32.const 3) (i32.const 0))
    drop
    (i32.load (i32.const 4)))

  ;; fd 4 は preopen 無し
  (func (export "prestat_get_fd4_ebadf") (result i32)
    (call $fd_prestat_get (i32.const 4) (i32.const 0)))

  (func (export "prestat_dir_name_ok") (result i32)
    (call $fd_prestat_dir_name (i32.const 3) (i32.const 50) (i32.const 1)))

  ;; "." を memory[50] に書いた後の最初の 1 byte
  (func (export "prestat_dir_name_byte0") (result i32)
    (call $fd_prestat_dir_name (i32.const 3) (i32.const 50) (i32.const 1))
    drop
    (i32.load8_u (i32.const 50)))

  ;; ============================================================
  ;; path_open
  ;; ============================================================

  ;; hello.txt を open → 成功 (後続テストの fd 番号を予測可能にするため close する)
  (func (export "open_hello_ok") (result i32)
    (local $err i32)
    (local.set $err
      (call $path_open
        (i32.const 3) (i32.const 0)
        (i32.const 100) (i32.const 9)
        (i32.const 0) (i64.const 0) (i64.const 0) (i32.const 0)
        (i32.const 1000)))
    (call $fd_close (i32.load (i32.const 1000)))
    drop
    (local.get $err))

  ;; 開いた fd は 4 (= preopen 直後の最初の slot)
  (func (export "open_hello_returned_fd") (result i32)
    (call $path_open
      (i32.const 3) (i32.const 0)
      (i32.const 100) (i32.const 9)
      (i32.const 0) (i64.const 0) (i64.const 0) (i32.const 0)
      (i32.const 1004))
    drop
    ;; close して片付け
    (call $fd_close (i32.load (i32.const 1004)))
    drop
    (i32.load (i32.const 1004)))

  ;; path traversal "../escape" は拒否される (EINVAL)
  (func (export "open_traversal_blocked") (result i32)
    (call $path_open
      (i32.const 3) (i32.const 0)
      (i32.const 200) (i32.const 9)
      (i32.const 0) (i64.const 0) (i64.const 0) (i32.const 0)
      (i32.const 1008)))

  ;; 存在しないファイルは EBADF (現実装は fopen 失敗を EBADF にマップ)
  (func (export "open_missing_file") (result i32)
    (call $path_open
      (i32.const 3) (i32.const 0)
      (i32.const 400) (i32.const 11)
      (i32.const 0) (i64.const 0) (i64.const 0) (i32.const 0)
      (i32.const 1012)))

  ;; ============================================================
  ;; fd_read: hello.txt の最初の 4 byte = "Hell" (LE 0x6C6C6548)
  ;; ============================================================

  (func (export "read_hello_first4") (result i32)
    (local $fd i32)
    (call $path_open
      (i32.const 3) (i32.const 0)
      (i32.const 100) (i32.const 9)
      (i32.const 0) (i64.const 0) (i64.const 0) (i32.const 0)
      (i32.const 1100))
    drop
    (local.set $fd (i32.load (i32.const 1100)))
    ;; iovec @ 2000: addr=2100, len=4
    (i32.store (i32.const 2000) (i32.const 2100))
    (i32.store (i32.const 2004) (i32.const 4))
    (call $fd_read (local.get $fd) (i32.const 2000) (i32.const 1) (i32.const 2010))
    drop
    (call $fd_close (local.get $fd))
    drop
    (i32.load (i32.const 2100)))

  ;; 読み込んだバイト数を確認 (= 4)
  (func (export "read_hello_count4") (result i32)
    (local $fd i32)
    (call $path_open
      (i32.const 3) (i32.const 0)
      (i32.const 100) (i32.const 9)
      (i32.const 0) (i64.const 0) (i64.const 0) (i32.const 0)
      (i32.const 1100))
    drop
    (local.set $fd (i32.load (i32.const 1100)))
    (i32.store (i32.const 2000) (i32.const 2100))
    (i32.store (i32.const 2004) (i32.const 4))
    (call $fd_read (local.get $fd) (i32.const 2000) (i32.const 1) (i32.const 2010))
    drop
    (call $fd_close (local.get $fd))
    drop
    (i32.load (i32.const 2010)))

  ;; ============================================================
  ;; fd_seek + fd_tell: hello.txt を SEEK_END すると tell=12
  ;; ============================================================

  (func (export "seek_end_tell12") (result i64)
    (local $fd i32)
    (call $path_open
      (i32.const 3) (i32.const 0)
      (i32.const 100) (i32.const 9)
      (i32.const 0) (i64.const 0) (i64.const 0) (i32.const 0)
      (i32.const 1100))
    drop
    (local.set $fd (i32.load (i32.const 1100)))
    ;; SEEK_END (whence=2), offset=0
    (call $fd_seek (local.get $fd) (i64.const 0) (i32.const 2) (i32.const 3000))
    drop
    (call $fd_close (local.get $fd))
    drop
    (i64.load (i32.const 3000)))

  ;; ============================================================
  ;; fd_pread: digits.bin の offset=2 から 4 byte = "2345" (LE 0x35343332)
  ;; ============================================================

  (func (export "pread_digits_offset2") (result i32)
    (local $fd i32)
    (call $path_open
      (i32.const 3) (i32.const 0)
      (i32.const 300) (i32.const 10)
      (i32.const 0) (i64.const 0) (i64.const 0) (i32.const 0)
      (i32.const 1100))
    drop
    (local.set $fd (i32.load (i32.const 1100)))
    (i32.store (i32.const 2000) (i32.const 2100))
    (i32.store (i32.const 2004) (i32.const 4))
    (call $fd_pread (local.get $fd) (i32.const 2000) (i32.const 1) (i64.const 2) (i32.const 2010))
    drop
    (call $fd_close (local.get $fd))
    drop
    (i32.load (i32.const 2100)))

  ;; ============================================================
  ;; fd_filestat_get: hello.txt size = 12
  ;; ============================================================

  (func (export "filestat_hello_size") (result i64)
    (local $fd i32)
    (call $path_open
      (i32.const 3) (i32.const 0)
      (i32.const 100) (i32.const 9)
      (i32.const 0) (i64.const 0) (i64.const 0) (i32.const 0)
      (i32.const 1100))
    drop
    (local.set $fd (i32.load (i32.const 1100)))
    (call $fd_filestat_get (local.get $fd) (i32.const 4000))
    drop
    (call $fd_close (local.get $fd))
    drop
    ;; size field at offset 32 in __wasi_filestat_t
    (i64.load offset=32 (i32.const 4000)))

  ;; ============================================================
  ;; path_filestat_get: digits.bin size = 8
  ;; ============================================================

  (func (export "path_filestat_digits_size") (result i64)
    (call $path_filestat_get
      (i32.const 3) (i32.const 0)
      (i32.const 300) (i32.const 10) (i32.const 4000))
    drop
    (i64.load offset=32 (i32.const 4000)))

  ;; filetype = REGULAR_FILE (=4)
  (func (export "path_filestat_digits_type") (result i32)
    (call $path_filestat_get
      (i32.const 3) (i32.const 0)
      (i32.const 300) (i32.const 10) (i32.const 4000))
    drop
    (i32.load8_u offset=16 (i32.const 4000)))

  ;; ============================================================
  ;; fd_fdstat_get: 開いたファイル fd の filetype = REGULAR_FILE
  ;; ============================================================

  (func (export "fdstat_opened_filetype") (result i32)
    (local $fd i32)
    (call $path_open
      (i32.const 3) (i32.const 0)
      (i32.const 100) (i32.const 9)
      (i32.const 0) (i64.const 0) (i64.const 0) (i32.const 0)
      (i32.const 1100))
    drop
    (local.set $fd (i32.load (i32.const 1100)))
    (call $fd_fdstat_get (local.get $fd) (i32.const 5000))
    drop
    (call $fd_close (local.get $fd))
    drop
    (i32.load8_u (i32.const 5000)))

  ;; preopened directory の filetype = DIRECTORY (= 3)
  (func (export "fdstat_preopen_filetype") (result i32)
    (call $fd_fdstat_get (i32.const 3) (i32.const 5000))
    drop
    (i32.load8_u (i32.const 5000)))
)

;; ============================================================
;; assert_return
;; ============================================================

;; preopen
(assert_return (invoke "prestat_get_fd3_ok")     (i32.const 0))
(assert_return (invoke "prestat_get_namelen")    (i32.const 1))
(assert_return (invoke "prestat_get_fd4_ebadf")  (i32.const 8))
(assert_return (invoke "prestat_dir_name_ok")    (i32.const 0))
(assert_return (invoke "prestat_dir_name_byte0") (i32.const 0x2E))   ;; '.'

;; path_open
(assert_return (invoke "open_hello_ok")           (i32.const 0))
(assert_return (invoke "open_hello_returned_fd")  (i32.const 4))
(assert_return (invoke "open_traversal_blocked")  (i32.const 28))    ;; EINVAL
(assert_return (invoke "open_missing_file")       (i32.const 44))    ;; ENOENT

;; fd_read: "Hell" little-endian = 0x6C 0x6C 0x65 0x48 → 0x6C6C6548
(assert_return (invoke "read_hello_first4")  (i32.const 0x6C6C6548))
(assert_return (invoke "read_hello_count4")  (i32.const 4))

;; fd_seek + fd_tell
(assert_return (invoke "seek_end_tell12")  (i64.const 12))

;; fd_pread: "2345" LE = 0x35 0x34 0x33 0x32 → 0x35343332
(assert_return (invoke "pread_digits_offset2")  (i32.const 0x35343332))

;; filestat
(assert_return (invoke "filestat_hello_size")        (i64.const 12))
(assert_return (invoke "path_filestat_digits_size")  (i64.const 8))
(assert_return (invoke "path_filestat_digits_type")  (i32.const 4))   ;; REGULAR_FILE

;; fdstat
(assert_return (invoke "fdstat_opened_filetype")  (i32.const 4))     ;; REGULAR_FILE
(assert_return (invoke "fdstat_preopen_filetype") (i32.const 3))     ;; DIRECTORY
