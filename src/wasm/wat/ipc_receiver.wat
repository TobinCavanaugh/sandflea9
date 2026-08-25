;; ipc_receiver.wat — waits for signals from sender, reads shared memory byte,
;; writes to stdout. On TERM signal, exits.
;;
;; Setup: same as sender — parent spawns both, creates shmem, calls
;; ipc_setup_send on each. Receiver calls ipc_setup_wait to get (shmem_va, peer_pid).

(module
  (import "env" "fd_write" (func $fd_write (param i32 i32 i32 i32) (result i32)))
  (import "env" "ipc_setup_wait"  (func $ipc_setup_wait  (param i32 i32) (result i32)))
  (import "env" "ipc_shmem_read_byte" (func $ipc_shmem_read_byte (param i32) (result i32)))
  (import "env" "ipc_signal_wait" (func $ipc_signal_wait (param i32) (result i32)))

  (memory 1)

  (data (i32.const 100) "[ipc_recv] ")
  (data (i32.const 120) "\n")

  ;; Offset 0:    shmem VA (u64, 8 bytes)
  ;; Offset 8:    peer PID  (i32, 4 bytes)
  ;; Offset 16:   byte from shmem (u8)
  ;; Offset 32:   __wasi_ciovec_t[3] for stdout {buf, len}
  ;; Offset 64:   nwritten (i32)

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

    ;; Setup iovs array:
    ;; iov 0: "[ipc_recv] " (offset 100, len 11)
    (i32.store (i32.const 32) (i32.const 100))
    (i32.store (i32.const 36) (i32.const 11))
    ;; iov 1: byte (offset 16, len 1)
    (i32.store (i32.const 40) (i32.const 16))
    (i32.store (i32.const 44) (i32.const 1))
    ;; iov 2: "\n" (offset 120, len 1)
    (i32.store (i32.const 48) (i32.const 120))
    (i32.store (i32.const 52) (i32.const 1))

    ;; 2. Receive loop: wait for DATA_READY(1) | TERM(2)
    (block $done
      (loop $again
        ;; Block until signal arrives
        (call $ipc_signal_wait (i32.const 3))  ;; 1|2 = DATA_READY|TERM
        local.set $sig

        ;; Check TERM (bit 1 = 2)
        (i32.and (local.get $sig) (i32.const 2))
        (if (then (br $done)))

        ;; Read byte from shared memory offset 0
        (call $ipc_shmem_read_byte (i32.const 0))
        local.set $byte

        ;; Check sentinel 0xFF (255)
        (i32.eq (local.get $byte) (i32.const 255))
        (if (then (br $done)))

        ;; Store byte at offset 16
        (i32.store8 (i32.const 16) (local.get $byte))

        ;; Write to stdout via fd_write
        (i32.eq (local.get $byte) (i32.const 10))
        (if
          (then
            ;; Byte is newline: write prefix + newline (iovs_len = 2)
            (call $fd_write
              (i32.const 1)     ;; fd = stdout
              (i32.const 32)    ;; iovs
              (i32.const 2)     ;; iovs_len
              (i32.const 64)    ;; nwritten
            )
            drop
          )
          (else
            ;; Other char: write prefix + char + newline (iovs_len = 3)
            (call $fd_write
              (i32.const 1)     ;; fd = stdout
              (i32.const 32)    ;; iovs
              (i32.const 3)     ;; iovs_len
              (i32.const 64)    ;; nwritten
            )
            drop
          )
        )

        (br $again)
      )
    )
  )
)
