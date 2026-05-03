(module
  ;; Imports from sandfleaOS kernel
  (import "env" "sys_open" (func $open (param i32) (result i32)))
  (import "env" "sys_read" (func $read (param i32 i32 i32) (result i32)))
  (import "env" "sys_close" (func $close (param i32) (result i32)))

  ;; Linear memory (1 page = 64KB)
  (memory 1)

  ;; Filename at offset 1024 (null terminated)
  (data (i32.const 1024) "a.txt\00")

  (func (export "entry") (result i32)
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

    ;; Check if read failed or returned 0
    (i32.le_s (local.get $bytes_read) (i32.const 0))
    if
      (call $close (local.get $fd))
      drop
      i32.const -2
      return
    end

    ;; 3. Grab the byte from memory before closing
    (i32.load8_u (i32.const 2048))
    local.set $result_byte

    ;; 4. Close the file
    (call $close (local.get $fd))
    drop

    ;; 5. Return the byte we read
    local.get $result_byte
  )
)
