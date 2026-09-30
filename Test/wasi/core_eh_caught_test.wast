;; core_eh_caught_test.wast
;;
;; Regression tests for the g_caught ledger (legacy rethrow bookkeeping,
;; 2026-07-10). catch/catch_all push the caught exception for rethrow N and
;; the handler's fall-through end pops it — but non-local exits (br/br_if/
;; br_table out of a handler, return, a throw that leaves the handler, or a
;; delegate whose target skips handler frames) used to leave stale entries,
;; so a later rethrow N (a lexical depth) re-raised the WRONG exception.
;;
;; Fixes under test: compile-time caught_pop insertion (unconditional br /
;; return / return_call* / throw_local / throw_ref_local / dispatch
;; terminator), taken-side trampolines for br_if / br_table, an npop operand
;; on rethrow_local, watermarks in call_eh / call_indirect_eh (and the
;; resume path), and no-track catch ops for try_table (which has no rethrow
;; and no structural pop point).

(module
  (tag $x)
  (tag $y)

  ;; (1) unconditional br out of a handler, then rethrow.
  (func (export "brout") (result i32)
    (try (result i32)
      (do
        (try (result i32)
          (do (throw $x))
          (catch_all
            (try (do (throw $y)) (catch_all (br 0)))  ;; br skips the pop
            (rethrow 0))))                             ;; lexically = $x
      (catch $x (i32.const 1))
      (catch $y (i32.const 2))))

  ;; (2) br_if out of a handler (taken pops via trampoline; not-taken must not pop).
  (func $brif (param i32) (result i32)
    (try (result i32)
      (do
        (try (result i32)
          (do (throw $x))
          (catch_all
            (block $b (result i32)
              (try (result i32)
                (do (throw $y))
                (catch_all
                  (i32.const 5)
                  (local.get 0)
                  (br_if $b)
                  drop
                  (i32.const 6))))
            drop
            (rethrow 0))))
      (catch $x (i32.const 1))
      (catch $y (i32.const 2))))
  (func (export "brif1") (result i32) (call $brif (i32.const 1)))
  (func (export "brif0") (result i32) (call $brif (i32.const 0)))

  ;; (3) return out of a handler; the caller's ledger must stay balanced
  ;;     (call_eh watermark also covers this across functions).
  (func $retout (result i32)
    (try (result i32)
      (do (throw $y))
      (catch_all (return (i32.const 9)))))
  (func (export "retout") (result i32)
    (try (result i32)
      (do
        (try (result i32)
          (do (throw $x))
          (catch_all
            (call $retout)
            drop
            (rethrow 0))))
      (catch $x (i32.const 1))
      (catch $y (i32.const 2))))

  ;; (4) throw out of a handler to an outer same-function try (dispatch
  ;;     terminator path): the skipped handler entry must be popped.
  (func (export "throwout") (result i32)
    (try (result i32)
      (do
        (try (result i32)
          (do
            (try (result i32)
              (do (throw $x))
              (catch_all (throw $y))))
          (catch_all
            (rethrow 0))))                ;; lexically = $y
      (catch $x (i32.const 1))
      (catch $y (i32.const 2))))

  ;; (5) a try_table catch inside a legacy handler must not pollute the
  ;;     ledger (no-track catch ops).
  (func (export "ttpollute") (result i32)
    (try (result i32)
      (do
        (try (result i32)
          (do (throw $x))
          (catch_all
            (block $h
              (try_table (catch_all $h)
                (throw $y))
              (unreachable))
            (rethrow 0))))
      (catch $x (i32.const 1))
      (catch $y (i32.const 2))))

  ;; (6) a try_table catch_ref inside a legacy handler: creating an exnref must
  ;;     not pollute the legacy rethrow ledger. catch_ref/catch_all_ref stash the
  ;;     caught exception in a separate g_exn array (exn_push), not g_caught, so a
  ;;     later rethrow of the outer legacy try still re-raises the outer exception.
  ;;     Pre-fix the catch_ref push landed in g_caught and shifted the rethrow's
  ;;     lexical index, so it re-threw $y (-> 2) instead of $x (-> 1). The outer
  ;;     try_table routes $x -> $got_x (1) and $y -> $got_y (2) to tell them apart.
  (func (export "refrethrow") (result i32)
    (block $got_x
      (block $got_y
        (try_table (catch $x $got_x) (catch $y $got_y)
          (try $outer
            (do (throw $x))
            (catch $x
              (block $b (result exnref)
                (try_table (result exnref) (catch_ref $y $b)
                  (throw $y)))
              drop
              (rethrow $outer))))
        (unreachable))
      (return (i32.const 2)))
    (i32.const 1))
)

(assert_return (invoke "brout") (i32.const 1))
(assert_return (invoke "brif1") (i32.const 1))
(assert_return (invoke "brif0") (i32.const 1))
(assert_return (invoke "retout") (i32.const 1))
(assert_return (invoke "throwout") (i32.const 2))
(assert_return (invoke "ttpollute") (i32.const 1))
(assert_return (invoke "refrethrow") (i32.const 1))
