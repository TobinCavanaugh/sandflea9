;; crashme.wat — adversarial WASM module that stress-tests kernel sandboxing.
;;
;; Tests:
;;  1. Invalid memory offsets passed to host functions — bounds-check validation
;;  2. Deep recursive call chain — exercises wasm3 interpreter stack
;;  3. Tight loop of host function calls — dispatch stability
;;  4. Multiple host functions in sequence — link + call correctness
;;
;; This module does NOT claim the compositor — it tests sandboxing without
;; taking over the display.  present() calls use invalid offsets that MUST
;; be rejected by the kernel's bounds checks.
;;
;; If the kernel is properly sandboxed, this module should return cleanly
;; (or trap within WASM, killing only itself).  It should NEVER page-fault
;; or double-fault the kernel.
;;
;; NOTE on recursion depth: each WASM function call uses ~200-400 bytes of
;; the kernel thread's C stack (wasm3 interpreter frame).  The kernel thread
;; stack is 256KB, minus overhead for the wasm3 dispatch, host calls, etc.
;; We use 200 levels (~80KB) to stay safely within the stack while still
;; exercising deep recursion.

(module
  ;; ── Compositor imports ────────────────────────────────────────────────
  ;; We import these but use them sparingly — only to test that calling
  ;; them with invalid args doesn't crash the kernel.
  (import "display" "getResolution" (func $getResolution (result i32)))
  (import "display" "present"       (func $present       (param i32) (result i32)))

  (memory 1)

  ;; ── Deep recursion (param depth, returns 0) ──────────────────────────
  ;; Each call adds one stack frame in wasm3's interpreter.
  (func $recurse (param i32) (result i32)
    (if (result i32) (i32.eqz (local.get 0))
      (then (i32.const 0))
      (else
        (drop (call $recurse (i32.sub (local.get 0) (i32.const 1))))
        (i32.const 0)
      )
    )
  )

  ;; ── Present with various offsets — all MUST be rejected ──────────────
  (func $try_present (param i32)
    ;; Call present and discard result — we just care it doesn't crash
    (drop (call $present (local.get 0)))
  )

  ;; ── Entry point ──────────────────────────────────────────────────────
  (func (export "_start") (local i32)  ;; local 0 = loop counter
    ;; ── Test 1: getResolution (always safe, no compositor needed) ─────
    (drop (call $getResolution))

    ;; ── Test 2: present with absurd offsets — must be rejected ─────────
    ;; Memory is 1 page (64KB).  Screen is ~8MB.  All of these should
    ;; fail the bounds check and return an error code, never crash.

    ;; Present with offset 0 — within memory but too small for screen
    (call $try_present (i32.const 0))

    ;; Present with offset 1 — within memory but too small
    (call $try_present (i32.const 1))

    ;; Present with offset 0xFFFFFFFF — absurd, integer overflow risk
    (call $try_present (i32.const 0xFFFFFFFF))

    ;; Present with offset 0x7FFFFFFF — large positive
    (call $try_present (i32.const 0x7FFFFFFF))

    ;; Present with offset 65536 — one WASM page (at the boundary)
    (call $try_present (i32.const 65536))

    ;; Present with offset 65535 — last valid byte in memory
    (call $try_present (i32.const 65535))

    ;; Present with offset 1000000 — well past memory
    (call $try_present (i32.const 1000000))

    ;; ── Test 3: Deep recursion — 200 levels ────────────────────────────
    ;; Each wasm3 frame uses ~200-400 bytes of C stack.  200 levels ≈ 80KB,
    ;; safely within the 256KB thread stack.
    (drop (call $recurse (i32.const 200)))

    ;; ── Test 4: Present again after recursion (tests stack recovery) ───
    (call $try_present (i32.const 0xFFFFFFFF))

    ;; ── Test 5: Tight loop — 50 present calls with invalid offsets ─────
    ;; Tests host function dispatch stability under load.
    (local.set 0 (i32.const 0))
    (loop $loop
      (call $try_present (i32.const 0xFFFFFFFF))
      (local.set 0 (i32.add (local.get 0) (i32.const 1)))
      (br_if $loop (i32.lt_s (local.get 0) (i32.const 50)))
    )

    ;; Done. Module returns cleanly — no compositor was claimed,
    ;; no valid blit was attempted, screen is untouched.
  )
)
