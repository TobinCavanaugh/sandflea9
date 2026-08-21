;; ipc_sender.wat — reads stdin byte-by-byte, writes each byte to shared
;; memory, signals the receiver. On EOF (read returns 0), signals TERM and exits.
;;
;; Setup: parent spawns sender + receiver, creates shmem, calls ipc_setup_send
;; on each. Sender calls ipc_setup_wait to get (shmem_va, peer_pid).

(module
  (import "env" "fd_read"  (func $fd_read  (param i32 i32 i32) (result i32)))
  (import "env" "fd_write" (func $fd_write (param i32 i32 i32 i32) (result i32)))
  (import "env" "ipc_setup_wait"  (func $ipc_setup_wait  (param i32 i32) (result i32)))
  (import "env" "ipc_signal_send" (func $ipc_signal_send (param i32 i32) (result i32)))

  (memory 1)

  ;; Offset 0:    shmem VA (u64, 8 bytes) — filled by ipc_setup_wait
  ;; Offset 8:    peer PID  (i32, 4 bytes)
  ;; Offset 16:   stdin read buffer (1 byte)
  ;; Offset 32:   __wasi_ciovec_t for stdout write {buf, len}
  ;; Offset 40:   nwritten (i32)

  (func (export "_start")
    (local $shmem_va i64)
    (local $peer_pid i32)
    (local $byte i32)

    ;; 1. Wait for parent setup
    (call $ipc_setup_wait (i32.const 0) (i32.const 8))
    drop
    (local.set $shmem_va (i64.load (i32.const 0)))
    (local.set $peer_pid  (i32.load (i32.const 8)))

    ;; 2. Read loop
    (block $done
      (loop $again
        ;; read stdin (fd 0) → offset 16, 1 byte
        (call $fd_read (i32.const 0) (i32.const 16) (i32.const 1))
        local.set $byte

        ;; EOF → exit
        (i32.le_s (local.get $byte) (i32.const 0))
        (if (then
          ;; Write sentinel 0xFF to shmem
          (i32.store8 (i32.wrap_i64 (local.get $shmem_va)) (i32.const 0xFF))
          ;; Signal TERM
          (call $ipc_signal_send (local.get $peer_pid) (i32.const 2))  ;; IPC_SIG_TERM = 1<<1 = 2
          drop
          (br $done)
        ))

        ;; Write byte to shared memory
        (i32.store8 (i32.wrap_i64 (local.get $shmem_va)) (local.get $byte))
        ;; Signal DATA_READY (bit 0)
        (call $ipc_signal_send (local.get $peer_pid) (i32.const 1))
        drop

        (br $again)
      )
    )
  )
)
