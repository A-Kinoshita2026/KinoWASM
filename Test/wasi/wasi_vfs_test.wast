;; wasi_vfs_test.wast
;;
;; wasi_set_vfs() で差し替えた in-memory モック VFS が WASI file 系関数で
;; 実際に呼ばれていることを検証する。
;;
;; testsuite_runner はこのテストの前に:
;;   wasi_set_vfs(&mock_vfs);
;;   wasi_add_preopen("/mock", ".");   // 実 path は "/mock" (実在しない)
;; を行う。モックは "vfs_mock.bin" のみ提供し、内容は 8 byte:
;;   "VFS-OK!" + 0xAB  → bytes: 'V','F','S','-','O','K','!',0xAB
;;
;; libc バックエンドの場合 path "/mock" は存在しないので path_open は EBADF。
;; モック VFS が呼ばれていれば path_open は成功し、固定 byte 列が読める。

(module
  (import "wasi_snapshot_preview1" "path_open"
    (func $path_open (param i32 i32 i32 i32 i32 i64 i64 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_close"
    (func $fd_close (param i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_read"
    (func $fd_read (param i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_filestat_get"
    (func $fd_filestat_get (param i32 i32) (result i32)))

  (memory (export "mem") 1)

  (data (i32.const 100) "vfs_mock.bin")     ;; 12 byte
  (data (i32.const 200) "real_file.bin")    ;; 13 byte (mock は知らない)

  ;; モック経由で open 成功
  (func (export "open_mock_ok") (result i32)
    (call $path_open
      (i32.const 3) (i32.const 0)
      (i32.const 100) (i32.const 12)
      (i32.const 0) (i64.const 0) (i64.const 0) (i32.const 0)
      (i32.const 1000)))

  ;; 先頭 4 byte = "VFS-" (LE: 'V'=0x56, 'F'=0x46, 'S'=0x53, '-'=0x2D)
  ;; → 0x2D534656
  (func (export "read_mock_first4") (result i32)
    (local $fd i32)
    (call $path_open
      (i32.const 3) (i32.const 0)
      (i32.const 100) (i32.const 12)
      (i32.const 0) (i64.const 0) (i64.const 0) (i32.const 0)
      (i32.const 1000))
    drop
    (local.set $fd (i32.load (i32.const 1000)))
    (i32.store (i32.const 2000) (i32.const 2100))
    (i32.store (i32.const 2004) (i32.const 4))
    (call $fd_read (local.get $fd) (i32.const 2000) (i32.const 1) (i32.const 2010))
    drop
    (call $fd_close (local.get $fd))
    drop
    (i32.load (i32.const 2100)))

  ;; size = 8
  (func (export "filestat_mock_size") (result i64)
    (local $fd i32)
    (call $path_open
      (i32.const 3) (i32.const 0)
      (i32.const 100) (i32.const 12)
      (i32.const 0) (i64.const 0) (i64.const 0) (i32.const 0)
      (i32.const 1000))
    drop
    (local.set $fd (i32.load (i32.const 1000)))
    (call $fd_filestat_get (local.get $fd) (i32.const 3000))
    drop
    (call $fd_close (local.get $fd))
    drop
    (i64.load offset=32 (i32.const 3000)))

  ;; モックが知らないファイルは EBADF
  (func (export "open_unknown_file") (result i32)
    (call $path_open
      (i32.const 3) (i32.const 0)
      (i32.const 200) (i32.const 13)
      (i32.const 0) (i64.const 0) (i64.const 0) (i32.const 0)
      (i32.const 1000)))
)

;; ============================================================
;; assert_return
;; ============================================================

(assert_return (invoke "open_mock_ok")        (i32.const 0))
(assert_return (invoke "read_mock_first4")    (i32.const 0x2D534656))   ;; "VFS-"
(assert_return (invoke "filestat_mock_size")  (i64.const 8))
(assert_return (invoke "open_unknown_file")   (i32.const 44))           ;; ENOENT
