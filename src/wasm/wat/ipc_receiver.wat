;; ipc_receiver.wat — waits for signals from sender, reads shared memory byte,
;; writes to stdout. On TERM signal, exits.
;;
;; Setup: same as sender — parent spawns both, creates shmem, calls
;; ipc_setup_send on each. Receiver calls ipc_setup_wait to get (shmem_va, peer_pid).

(module
  (import "env" "fd_write" (func $fd_write (param i32 i32 i32 i32) (result i32)))
  (import "env" "ipc_setup_wait"  (func $ipc_setup_wait  (param i32 i32) (result i32)))
  (import "env" "ipc_signal_wait" (func $ipc_signal_wait (param i32) (result i32)))

  (memory 1)

  ;; Offset 0:    shmem VA (u64, 8 bytes)
  ;; Offset 8:    peer PID  (i32, 4 bytes)
  ;; Offset 16:   byte from shmem (u8)
  ;; Offset 32:   __wasi_ciovec_t for stdout {buf, len}
  ;; Offset 40:   nwritten (i32)

  (func (export "_start")
    (local $shmem_va i64)
    (local $peer_pid i32)
    (local $byte     i32)
    (local $sig      i32)

    ;; 1. Wait for parent setup
    (call $ipc_setup_wait (i32.const 0) (i32.const 8))
    drop
    (local.set $shmem_va (i64.load (i32.const 0)))
    (local.set $peer_pid  (i32.load (i32.const 8)))

    ;; 2. Receive loop: wait for DATA_READY(1) | TERM(2)
    (block $done
      (loop $again
        ;; Block until signal arrives
        (call $ipc_signal_wait (i32.const 3))  ;; 1|2 = DATA_READY|TERM
        local.set $sig

        ;; Check TERM
        (i32.and (local.get $sig) (i32.const 2))  ;; IPC_SIG_TERM
        (if (then (br $done)))

        ;; Read byte from shared memory
        (i32.load8_u (i32.wrap_i64 (local.get $shmem_va)))
        local.set $byte

        ;; Write to stdout via fd_write(1, iovs→offset 32, 1, nwritten→offset 40)
        (i32.store (i32.const 32) (i32.const 16))   ;; iov.buf = offset 16
        (i32.store (i32.const 36) (i32.const 1))     ;; iov.len = 1
        (i32.store8 (i32.const 16) (local.get $byte)) ;; store byte at offset 16

        (call $fd_write
          (i32.const 1)     ;; fd = stdout
          (i32.const 32)    ;; iovs
          (i32.const 1)     ;; iovs_len
          (i32.const 40)    ;; nwritten
        )
        drop

        (br $again)
      )
    )
  )
)
