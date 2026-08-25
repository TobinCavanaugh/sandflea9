;; ipc_receiver.wat — waits for signals from sender, reads shared memory byte,
;; writes to stdout. On TERM signal, exits.
;;
;; Setup:
;; If argv[1] is provided, attaches to that shm_id.
;; If not provided, creates a new shm_id via ipc_shm_create(1).
;; Prints its PID and shm_id to stdout so the sender knows where to connect.

(module
  (import "env" "fd_write" (func $fd_write (param i32 i32 i32 i32) (result i32)))
  (import "env" "get_arg_count" (func $get_arg_count (result i32)))
  (import "env" "get_arg_i32" (func $get_arg_i32 (param i32) (result i32)))
  (import "env" "ipc_get_pid" (func $ipc_get_pid (result i32)))
  (import "env" "ipc_shm_create" (func $ipc_shm_create (param i32) (result i32)))
  (import "env" "ipc_shm_attach" (func $ipc_shm_attach (param i32) (result i32)))
  (import "env" "ipc_shm_detach" (func $ipc_shm_detach (param i32) (result i32)))
  (import "env" "ipc_shm_read_byte" (func $ipc_shm_read_byte (param i32 i32) (result i32)))
  (import "env" "ipc_signal_wait" (func $ipc_signal_wait (param i32) (result i32)))

  (memory 1)

  ;; Banner strings
  (data (i32.const 100) "[ipc_recv] PID=")
  (data (i32.const 120) ", shm_id=")
  (data (i32.const 140) " (waiting for sender...)\n")
  (data (i32.const 180) "[ipc_recv] ")
  (data (i32.const 200) "\n")
  (data (i32.const 220) "[ipc_recv] Receiver exiting cleanly.\n")

  ;; Offset 300: PID string buffer
  ;; Offset 320: shm_id string buffer
  ;; Offset 350: byte from shmem (u8)
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
    (local $my_pid     i32)
    (local $shm_id     i32)
    (local $handle     i32)
    (local $pid_len    i32)
    (local $shm_len    i32)
    (local $byte       i32)
    (local $sig        i32)

    ;; 1. Get own PID
    (call $ipc_get_pid)
    local.set $my_pid

    ;; 2. Determine shm_id: from argv[1] if provided, else create region
    (i32.ge_s (call $get_arg_count) (i32.const 2))
    (if
      (then
        (call $get_arg_i32 (i32.const 1))
        local.set $shm_id
        (call $ipc_shm_attach (local.get $shm_id))
        local.set $handle
      )
      (else
        (call $ipc_shm_create (i32.const 1))
        local.set $shm_id
        (local.set $handle (i32.const 0))
      )
    )

    ;; 3. Format PID and shm_id into ASCII
    (call $store_u32 (local.get $my_pid) (i32.const 300))
    local.set $pid_len

    (call $store_u32 (local.get $shm_id) (i32.const 320))
    local.set $shm_len

    ;; Setup iovs for startup banner:
    ;; iov 0: "[ipc_recv] PID=" (offset 100, len 15)
    (i32.store (i32.const 400) (i32.const 100))
    (i32.store (i32.const 404) (i32.const 15))
    ;; iov 1: PID digits (offset 300, len $pid_len)
    (i32.store (i32.const 408) (i32.const 300))
    (i32.store (i32.const 412) (local.get $pid_len))
    ;; iov 2: ", shm_id=" (offset 120, len 9)
    (i32.store (i32.const 416) (i32.const 120))
    (i32.store (i32.const 420) (i32.const 9))
    ;; iov 3: shm_id digits (offset 320, len $shm_len)
    (i32.store (i32.const 424) (i32.const 320))
    (i32.store (i32.const 428) (local.get $shm_len))
    ;; iov 4: " (waiting for sender...)\n" (offset 140, len 25)
    (i32.store (i32.const 432) (i32.const 140))
    (i32.store (i32.const 436) (i32.const 25))

    ;; Print banner to stdout
    (call $fd_write
      (i32.const 1)
      (i32.const 400)
      (i32.const 5)
      (i32.const 480)
    )
    drop

    ;; Setup iovs for receiving characters:
    ;; iov 0: "[ipc_recv] " (offset 180, len 11)
    (i32.store (i32.const 400) (i32.const 180))
    (i32.store (i32.const 404) (i32.const 11))
    ;; iov 1: byte (offset 350, len 1)
    (i32.store (i32.const 408) (i32.const 350))
    (i32.store (i32.const 412) (i32.const 1))
    ;; iov 2: "\n" (offset 200, len 1)
    (i32.store (i32.const 416) (i32.const 200))
    (i32.store (i32.const 420) (i32.const 1))

    ;; 4. Receive loop: wait for DATA_READY(1) | TERM(2)
    (block $done
      (loop $again
        (call $ipc_signal_wait (i32.const 3))
        local.set $sig

        ;; Check TERM (bit 1 = 2)
        (i32.and (local.get $sig) (i32.const 2))
        (if (then (br $done)))

        ;; Read byte from shared memory handle at offset 0
        (call $ipc_shm_read_byte (local.get $handle) (i32.const 0))
        local.set $byte

        ;; Check sentinel 0xFF (255)
        (i32.eq (local.get $byte) (i32.const 255))
        (if (then (br $done)))

        ;; Store byte at offset 350
        (i32.store8 (i32.const 350) (local.get $byte))

        ;; Print received byte to stdout
        (i32.eq (local.get $byte) (i32.const 10))
        (if
          (then
            ;; Byte is newline: write prefix + newline (iovs_len = 2)
            (call $fd_write
              (i32.const 1)
              (i32.const 400)
              (i32.const 2)
              (i32.const 480)
            )
            drop
          )
          (else
            ;; Character: write prefix + char + newline (iovs_len = 3)
            (call $fd_write
              (i32.const 1)
              (i32.const 400)
              (i32.const 3)
              (i32.const 480)
            )
            drop
          )
        )

        (br $again)
      )
    )

    ;; 5. Print exit message and clean detach
    (i32.store (i32.const 400) (i32.const 220))
    (i32.store (i32.const 404) (i32.const 37))
    (call $fd_write (i32.const 1) (i32.const 400) (i32.const 1) (i32.const 480))
    drop

    (call $ipc_shm_detach (local.get $handle))
    drop
  )
)
