(module
  ;; Imports from sandfleaOS kernel
  (import "env" "fd_open" (func $open (param i32) (result i32)))
  (import "env" "fd_read" (func $read (param i32 i32 i32) (result i32)))
  (import "env" "fd_close" (func $close (param i32) (result i32)))
  ;; fd_write(fd, iovs_ptr, iovs_len, nwritten_ptr)
  (import "env" "fd_write" (func $write (param i32 i32 i32 i32) (result i32)))

  ;; Linear memory (1 page = 64KB)
  (memory 1)

  ;; Filename at offset 1024
  (data (i32.const 1024) "a.txt\00")

  ;; String to write at offset 3072
  (data (i32.const 3072) "Hello from WASM!")

  (func (export "_start") (result i32)
    (local $fd i32)
    (local $bytes_read i32)
    (local $result_byte i32)

    ;; 1. Open the file
    (call $open (i32.const 1024))
    local.set $fd

    ;; Check if open failed (fd < 0)
    (i32.lt_s (local.get $fd) (i32.const 0))
    if
      i32.const -1
      return
    end

    ;; 2. Read first character into memory at offset 2048
    (call $read
      (local.get $fd)
      (i32.const 2048) ;; Buffer offset
      (i32.const 1)    ;; Read 1 byte
    )
    local.set $bytes_read

    ;; 3. Prepare __wasi_ciovec_t at offset 3088
    ;; Structure: [u32 buf_ptr] [u32 buf_len]
    (i32.store (i32.const 3088) (i32.const 3072)) ;; Pointer to "Hello..."
    (i32.store (i32.const 3092) (i32.const 16))   ;; Length of string

    ;; 4. Write to the file
    (call $write
      (local.get $fd)
      (i32.const 3088) ;; Pointer to the iovs array
      (i32.const 1)    ;; Number of iovs (just one)
      (i32.const 3100) ;; Offset to store nwritten result
    )
    drop ;; Ignoring the errno for now

    ;; Grab the byte we read earlier
    (i32.load8_u (i32.const 2048))
    local.set $result_byte

    ;; 5. Close the file
    (call $close (local.get $fd))
    drop

    ;; Return the byte we read
    local.get $result_byte
  )
)
