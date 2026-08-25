;; ipc_sender.wat — reads stdin byte-by-byte, writes each byte to shared
;; memory, signals the receiver. On EOF, signals TERM, detaches, and exits.
;;
;; Setup: reads shm_id from argv[1] and peer_pid from argv[2],
;; attaches to shm_id via ipc_shm_attach(shm_id) -> gets local handle.

(module
  (import "env" "fd_read"  (func $fd_read  (param i32 i32 i32) (result i32)))
  (import "env" "get_arg_i32" (func $get_arg_i32 (param i32) (result i32)))
  (import "env" "ipc_shm_attach" (func $ipc_shm_attach (param i32) (result i32)))
  (import "env" "ipc_shm_detach" (func $ipc_shm_detach (param i32) (result i32)))
  (import "env" "ipc_shm_write_byte" (func $ipc_shm_write_byte (param i32 i32 i32) (result i32)))
  (import "env" "ipc_signal_send" (func $ipc_signal_send (param i32 i32) (result i32)))

  (memory 1)

  ;; Offset 16:   stdin read buffer (1 byte)

  (func (export "_start")
    (local $shm_id     i32)
    (local $peer_pid   i32)
    (local $handle     i32)
    (local $nread      i32)
    (local $byte       i32)

    ;; 1. Get shm_id from argv[1] and peer_pid from argv[2]
    (call $get_arg_i32 (i32.const 1))
    local.set $shm_id

    (call $get_arg_i32 (i32.const 2))
    local.set $peer_pid

    ;; 2. Attach to shared memory region
    (call $ipc_shm_attach (local.get $shm_id))
    local.set $handle

    ;; 3. Read loop
    (block $done
      (loop $again
        ;; read stdin (fd 0) → offset 16, 1 byte
        (call $fd_read (i32.const 0) (i32.const 16) (i32.const 1))
        local.set $nread

        ;; EOF or error (nread <= 0) → exit
        (i32.le_s (local.get $nread) (i32.const 0))
        (if (then
          ;; Write sentinel 0xFF to shmem handle at offset 0
          (call $ipc_shm_write_byte (local.get $handle) (i32.const 0) (i32.const 255))
          drop
          ;; Signal TERM (bit 1 = 2)
          (call $ipc_signal_send (local.get $peer_pid) (i32.const 2))
          drop
          (br $done)
        ))

        ;; Load the byte that was read into memory at offset 16
        (local.set $byte (i32.load8_u (i32.const 16)))

        ;; Write byte to shared memory handle at offset 0
        (call $ipc_shm_write_byte (local.get $handle) (i32.const 0) (local.get $byte))
        drop

        ;; Signal DATA_READY (bit 0 = 1)
        (call $ipc_signal_send (local.get $peer_pid) (i32.const 1))
        drop

        (br $again)
      )
    )

    ;; 4. Clean detach
    (call $ipc_shm_detach (local.get $handle))
    drop
  )
)
