;; wasi_test.wast
;;
;; KinoRuntime の WASI snapshot_preview1 実装に対する回帰テスト。
;; extrafunction.c で実装している WASI host 関数を WAT モジュールから直接
;; import で呼び、戻り値 (errno) と memory 書き込み内容を assert_return で
;; 検証する。
;;
;; 前提: testsuite_runner は起動時に register_standard_func() を呼ぶため、
;;       WASI 関数は登録済みの状態でこの module をロードできる。
;;
;; 参照: docs/Host-Functions.md §7 の関数一覧
;;       extrafunction.c の Phase 1 実装

(module
  ;; ── WASI Imports (memory より前に置く必要あり) ──
  (import "wasi_snapshot_preview1" "random_get"
    (func $random_get (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "clock_time_get"
    (func $clock_time_get (param i32 i64 i32) (result i32)))
  (import "wasi_snapshot_preview1" "clock_res_get"
    (func $clock_res_get (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_prestat_get"
    (func $fd_prestat_get (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_prestat_dir_name"
    (func $fd_prestat_dir_name (param i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_close"
    (func $fd_close (param i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_fdstat_get"
    (func $fd_fdstat_get (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_seek"
    (func $fd_seek (param i32 i64 i32 i32) (result i32)))
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
  ;; random_get
  ;; ===========================================================

  ;; len=0 は何もせず errno 0 で返るべき
  (func (export "random_get_zero_len") (result i32)
    (call $random_get (i32.const 0) (i32.const 0)))

  ;; 32 byte の通常呼び出し → errno 0
  (func (export "random_get_basic") (result i32)
    (call $random_get (i32.const 0) (i32.const 32)))

  ;; 2 回呼び出して別アドレスに書き、内容が異なることを確認 (4 byte XOR が
  ;; 0 でない)。確率 1/2^32 で誤検知の可能性はあるが実用上問題なし。
  (func (export "random_get_differs") (result i32)
    (call $random_get (i32.const 0) (i32.const 4))
    drop
    (call $random_get (i32.const 16) (i32.const 4))
    drop
    (i32.ne
      (i32.xor
        (i32.load (i32.const 0))
        (i32.load (i32.const 16)))
      (i32.const 0)))

  ;; ===========================================================
  ;; clock_time_get
  ;; ===========================================================

  (func (export "clock_time_realtime_ok") (result i32)
    (call $clock_time_get (i32.const 0) (i64.const 1000) (i32.const 0)))

  (func (export "clock_time_monotonic_ok") (result i32)
    (call $clock_time_get (i32.const 1) (i64.const 1000) (i32.const 0)))

  ;; 不正な clockid は EINVAL (28) を返す
  (func (export "clock_time_invalid_id") (result i32)
    (call $clock_time_get (i32.const 99) (i64.const 1000) (i32.const 0)))

  ;; REALTIME (1970 起算 ns) の上位 32 bit が非 0 であること。
  ;; 2001 年以降であれば 1e9 sec * 1e9 ns/sec ≒ 1e18 ns = 0x0DE0_B6B3_xxxxxxxx
  ;; なので上位 32 bit は >= 1。
  (func (export "clock_time_realtime_high32_nonzero") (result i32)
    (call $clock_time_get (i32.const 0) (i64.const 1000) (i32.const 0))
    drop
    (i32.gt_u (i32.load offset=4 (i32.const 0)) (i32.const 0)))

  ;; MONOTONIC は単調非減少。2 回読んで第 2 回 >= 第 1 回。
  (func (export "clock_time_monotonic_nondecreasing") (result i32)
    (local $t1 i64)
    (local $t2 i64)
    (call $clock_time_get (i32.const 1) (i64.const 1000) (i32.const 0))
    drop
    (local.set $t1 (i64.load (i32.const 0)))
    (call $clock_time_get (i32.const 1) (i64.const 1000) (i32.const 8))
    drop
    (local.set $t2 (i64.load (i32.const 8)))
    (i64.ge_u (local.get $t2) (local.get $t1)))

  ;; ===========================================================
  ;; clock_res_get
  ;; ===========================================================

  (func (export "clock_res_get_ok") (result i32)
    (call $clock_res_get (i32.const 0) (i32.const 0)))

  ;; 実装は常に 1ns を返す
  (func (export "clock_res_get_value") (result i64)
    (call $clock_res_get (i32.const 0) (i32.const 0))
    drop
    (i64.load (i32.const 0)))

  (func (export "clock_res_get_invalid_id") (result i32)
    (call $clock_res_get (i32.const 99) (i32.const 0)))

  ;; ===========================================================
  ;; fd_prestat_get / fd_prestat_dir_name
  ;; ===========================================================

  ;; preopened FD は無いので任意の fd で EBADF (8)
  (func (export "fd_prestat_get_fd3") (result i32)
    (call $fd_prestat_get (i32.const 3) (i32.const 0)))

  (func (export "fd_prestat_get_fd0") (result i32)
    (call $fd_prestat_get (i32.const 0) (i32.const 0)))

  (func (export "fd_prestat_dir_name_fd3") (result i32)
    (call $fd_prestat_dir_name (i32.const 3) (i32.const 0) (i32.const 16)))

  ;; ===========================================================
  ;; fd_close
  ;; ===========================================================

  ;; 現状の実装は常に成功を返す (no-op)
  (func (export "fd_close_fd3") (result i32)
    (call $fd_close (i32.const 3)))

  (func (export "fd_close_stdout") (result i32)
    (call $fd_close (i32.const 1)))

  ;; ===========================================================
  ;; fd_fdstat_get
  ;; ===========================================================

  ;; fd 0/1/2 (stdin/stdout/stderr) は CHARACTER_DEVICE として成功
  (func (export "fd_fdstat_get_stdin") (result i32)
    (call $fd_fdstat_get (i32.const 0) (i32.const 0)))

  (func (export "fd_fdstat_get_stdout") (result i32)
    (call $fd_fdstat_get (i32.const 1) (i32.const 0)))

  (func (export "fd_fdstat_get_stderr") (result i32)
    (call $fd_fdstat_get (i32.const 2) (i32.const 0)))

  ;; fd 3 以降は EBADF (8)
  (func (export "fd_fdstat_get_fd3") (result i32)
    (call $fd_fdstat_get (i32.const 3) (i32.const 0)))

  ;; stdin の filetype が CHARACTER_DEVICE (= 2) であること。
  ;; __wasi_fdstat_t の最初の byte が fs_filetype。
  (func (export "fd_fdstat_get_stdin_filetype") (result i32)
    (call $fd_fdstat_get (i32.const 0) (i32.const 0))
    drop
    (i32.load8_u (i32.const 0)))

  ;; ===========================================================
  ;; fd_seek
  ;; ===========================================================

  ;; stdin/stdout/stderr は seek 不能 → EIO/ESPIPE (29) を返す現実装
  (func (export "fd_seek_stdout") (result i32)
    (call $fd_seek (i32.const 1) (i64.const 0) (i32.const 0) (i32.const 0)))

  ;; ===========================================================
  ;; args_sizes_get / args_get (現状スタブで 0 件)
  ;; ===========================================================

  (func (export "args_sizes_get_ok") (result i32)
    (call $args_sizes_get (i32.const 0) (i32.const 4)))

  (func (export "args_sizes_count_zero") (result i32)
    (call $args_sizes_get (i32.const 0) (i32.const 4))
    drop
    (i32.load (i32.const 0)))

  (func (export "args_sizes_buf_size_zero") (result i32)
    (call $args_sizes_get (i32.const 0) (i32.const 4))
    drop
    (i32.load (i32.const 4)))

  (func (export "args_get_ok") (result i32)
    (call $args_get (i32.const 0) (i32.const 0)))

  ;; ===========================================================
  ;; environ_sizes_get / environ_get (同上)
  ;; ===========================================================

  (func (export "environ_sizes_get_ok") (result i32)
    (call $environ_sizes_get (i32.const 0) (i32.const 4)))

  (func (export "environ_sizes_count_zero") (result i32)
    (call $environ_sizes_get (i32.const 0) (i32.const 4))
    drop
    (i32.load (i32.const 0)))

  (func (export "environ_get_ok") (result i32)
    (call $environ_get (i32.const 0) (i32.const 0)))
)

;; ============================================================
;; assert_return: 期待値で WASI 関数の戻り値を検証
;; ============================================================

;; ── random_get ──
(assert_return (invoke "random_get_zero_len")  (i32.const 0))
(assert_return (invoke "random_get_basic")     (i32.const 0))
(assert_return (invoke "random_get_differs")   (i32.const 1))

;; ── clock_time_get ──
(assert_return (invoke "clock_time_realtime_ok")              (i32.const 0))
(assert_return (invoke "clock_time_monotonic_ok")             (i32.const 0))
(assert_return (invoke "clock_time_invalid_id")               (i32.const 28))
(assert_return (invoke "clock_time_realtime_high32_nonzero")  (i32.const 1))
(assert_return (invoke "clock_time_monotonic_nondecreasing")  (i32.const 1))

;; ── clock_res_get ──
(assert_return (invoke "clock_res_get_ok")           (i32.const 0))
(assert_return (invoke "clock_res_get_value")        (i64.const 1))
(assert_return (invoke "clock_res_get_invalid_id")   (i32.const 28))

;; ── fd_prestat_get / dir_name ──
(assert_return (invoke "fd_prestat_get_fd3")           (i32.const 8))
(assert_return (invoke "fd_prestat_get_fd0")           (i32.const 8))
(assert_return (invoke "fd_prestat_dir_name_fd3")      (i32.const 8))

;; ── fd_close ──
;; fd 0/1/2 は no-op で成功、それ以外で preopen 登録なしなら EBADF
(assert_return (invoke "fd_close_fd3")     (i32.const 8))
(assert_return (invoke "fd_close_stdout")  (i32.const 0))

;; ── fd_fdstat_get ──
(assert_return (invoke "fd_fdstat_get_stdin")           (i32.const 0))
(assert_return (invoke "fd_fdstat_get_stdout")          (i32.const 0))
(assert_return (invoke "fd_fdstat_get_stderr")          (i32.const 0))
(assert_return (invoke "fd_fdstat_get_fd3")             (i32.const 8))
(assert_return (invoke "fd_fdstat_get_stdin_filetype")  (i32.const 2))

;; ── fd_seek ──
(assert_return (invoke "fd_seek_stdout") (i32.const 29))

;; ── args_sizes_get / args_get ──
(assert_return (invoke "args_sizes_get_ok")        (i32.const 0))
(assert_return (invoke "args_sizes_count_zero")    (i32.const 0))
(assert_return (invoke "args_sizes_buf_size_zero") (i32.const 0))
(assert_return (invoke "args_get_ok")              (i32.const 0))

;; ── environ_sizes_get / environ_get ──
(assert_return (invoke "environ_sizes_get_ok")     (i32.const 0))
(assert_return (invoke "environ_sizes_count_zero") (i32.const 0))
(assert_return (invoke "environ_get_ok")           (i32.const 0))
