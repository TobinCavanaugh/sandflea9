;; ipc_sender.wat — reads stdin byte-by-byte, writes each byte to shared
;; memory, signals the receiver. On EOF, signals TERM, detaches, and exits.
;;
;; Usage: wasm ipc_sender.wasm <shm_id> <target_pid>

(module
  (import "env" "fd_read"  (func $fd_read  (param i32 i32 i32) (result i32)))
  (import "env" "fd_write" (func $fd_write (param i32 i32 i32 i32) (result i32)))
  (import "env" "get_arg_count" (func $get_arg_count (result i32)))
  (import "env" "get_arg_i32" (func $get_arg_i32 (param i32) (result i32)))
  (import "env" "ipc_shm_attach" (func $ipc_shm_attach (param i32) (result i32)))
  (import "env" "ipc_shm_detach" (func $ipc_shm_detach (param i32) (result i32)))
  (import "env" "ipc_shm_write_byte" (func $ipc_shm_write_byte (param i32 i32 i32) (result i32)))
  (import "env" "ipc_signal_send" (func $ipc_signal_send (param i32 i32) (result i32)))

  (memory 1)

  ;; Banner & Usage strings
  (data (i32.const 100) "[ipc_send] Attached to shm_id=")
  (data (i32.const 132) ", target PID=")
  (data (i32.const 150) " (type to send, Ctrl+D to finish)\n")
  (data (i32.const 200) "[ipc_send] Usage: wasm ipc_sender.wasm <shm_id> <target_pid>\n")
  (data (i32.const 270) "[ipc_send] Sender exiting cleanly.\n")

  ;; Offset 300: shm_id string buffer
  ;; Offset 320: target PID string buffer
  ;; Offset 350: stdin read buffer (1 byte)
  ;; Offset 400: __wasi_ciovec_t array {buf, len}
  ;; Offset 480: nwritten (i32)

  ;; Integer to ASCII formatter: returns length written to $buf
  (func $store_u32 (param $val i32) (param $buf i32) (result i32)
    (local $len i32)
    (local $tmp i32)
    (local $i i32)
    (local $j i32)
    (local $c i32)

    (i32.eqz (local.get $val))
    (if (then
      (i32.store8 (local.get $buf) (i32.const 48))
      (return (i32.const 1))
    ))

    (local.set $tmp (local.get $val))
    (local.set $len (i32.const 0))
    (loop $count_loop
      (i32.gt_u (local.get $tmp) (i32.const 0))
      (if (then
        (i32.store8
          (i32.add (local.get $buf) (local.get $len))
          (i32.add (i32.rem_u (local.get $tmp) (i32.const 10)) (i32.const 48))
        )
        (local.set $tmp (i32.div_u (local.get $tmp) (i32.const 10)))
        (local.set $len (i32.add (local.get $len) (i32.const 1)))
        (br $count_loop)
      ))
    )

    ;; Reverse string in place
    (local.set $i (i32.const 0))
    (local.set $j (i32.sub (local.get $len) (i32.const 1)))
    (loop $rev_loop
      (i32.lt_s (local.get $i) (local.get $j))
      (if (then
        (local.set $c (i32.load8_u (i32.add (local.get $buf) (local.get $i))))
        (i32.store8
          (i32.add (local.get $buf) (local.get $i))
          (i32.load8_u (i32.add (local.get $buf) (local.get $j)))
        )
        (i32.store8
          (i32.add (local.get $buf) (local.get $j))
          (local.get $c)
        )
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (local.set $j (i32.sub (local.get $j) (i32.const 1)))
        (br $rev_loop)
      ))
    )

    (local.get $len)
  )

  (func (export "_start")
    (local $shm_id     i32)
    (local $peer_pid   i32)
    (local $handle     i32)
    (local $shm_len    i32)
    (local $pid_len    i32)
    (local $nread      i32)
    (local $byte       i32)

    ;; 1. Check argc (must have at least argv[1] = shm_id and argv[2] = peer_pid)
    (i32.lt_s (call $get_arg_count) (i32.const 3))
    (if
      (then
        ;; Print usage string
        (i32.store (i32.const 400) (i32.const 200))
        (i32.store (i32.const 404) (i32.const 62))
        (call $fd_write (i32.const 1) (i32.const 400) (i32.const 1) (i32.const 480))
        drop
        (return)
      )
    )

    ;; 2. Parse arguments
    (call $get_arg_i32 (i32.const 1))
    local.set $shm_id

    (call $get_arg_i32 (i32.const 2))
    local.set $peer_pid

    ;; 3. Attach to shared memory region
    (call $ipc_shm_attach (local.get $shm_id))
    local.set $handle

    ;; 4. Format shm_id and target PID
    (call $store_u32 (local.get $shm_id) (i32.const 300))
    local.set $shm_len

    (call $store_u32 (local.get $peer_pid) (i32.const 320))
    local.set $pid_len

    ;; Setup iovs for startup banner:
    ;; iov 0: "[ipc_send] Attached to shm_id=" (offset 100, len 30)
    (i32.store (i32.const 400) (i32.const 100))
    (i32.store (i32.const 404) (i32.const 30))
    ;; iov 1: shm_id string (offset 300, len $shm_len)
    (i32.store (i32.const 408) (i32.const 300))
    (i32.store (i32.const 412) (local.get $shm_len))
    ;; iov 2: ", target PID=" (offset 132, len 13)
    (i32.store (i32.const 416) (i32.const 132))
    (i32.store (i32.const 420) (i32.const 13))
    ;; iov 3: PID string (offset 320, len $pid_len)
    (i32.store (i32.const 424) (i32.const 320))
    (i32.store (i32.const 428) (local.get $pid_len))
    ;; iov 4: " (type to send, Ctrl+D to finish)\n" (offset 150, len 34)
    (i32.store (i32.const 432) (i32.const 150))
    (i32.store (i32.const 436) (i32.const 34))

    ;; Print banner to stdout
    (call $fd_write (i32.const 1) (i32.const 400) (i32.const 5) (i32.const 480))
    drop

    ;; 5. Read loop
    (block $done
      (loop $again
        ;; read stdin (fd 0) → offset 350, 1 byte
        (call $fd_read (i32.const 0) (i32.const 350) (i32.const 1))
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

        ;; Load the byte that was read into memory at offset 350
        (local.set $byte (i32.load8_u (i32.const 350)))

        ;; Write byte to shared memory handle at offset 0
        (call $ipc_shm_write_byte (local.get $handle) (i32.const 0) (local.get $byte))
        drop

        ;; Signal DATA_READY (bit 0 = 1)
        (call $ipc_signal_send (local.get $peer_pid) (i32.const 1))
        drop

        (br $again)
      )
    )

    ;; 6. Print exit message and clean detach
    (i32.store (i32.const 400) (i32.const 270))
    (i32.store (i32.const 404) (i32.const 35))
    (call $fd_write (i32.const 1) (i32.const 400) (i32.const 1) (i32.const 480))
    drop

    (call $ipc_shm_detach (local.get $handle))
    drop
  )
)
