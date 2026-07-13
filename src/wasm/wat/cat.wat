(module
  ;; Imports from sandfleaOS kernel
  (import "env" "fd_open" (func $open (param i32) (result i32)))
  (import "env" "fd_read" (func $read (param i32 i32 i32) (result i32)))
  (import "env" "fd_close" (func $close (param i32) (result i32)))
  ;; fd_write(fd, iovs_ptr, iovs_len, nwritten_ptr)
  (import "env" "fd_write" (func $write (param i32 i32 i32 i32) (result i32)))
  
  (import "env" "get_arg_count" (func $get_arg_count (result i32)))
  (import "env" "get_arg" (func $get_arg (param i32 i32 i32) (result i32)))

  ;; Linear memory (1 page = 64KB)
  (memory 1)

  ;; Memory layout:
  ;; Offset 1024: Buffer to store command line argument (path)
  ;; Offset 2048: Buffer for reading file/stdin content (e.g. 1024 bytes)
  ;; Offset 3072: __wasi_ciovec_t for fd_write
  ;; Offset 3088: nwritten result
  ;; Offset 3100: Error messages
  (data (i32.const 3100) "Unable to open file\n")

  (func (export "_start") (result i32)
    (local $argc i32)
    (local $fd i32)
    (local $bytes_read i32)
    (local $arg_len i32)

    ;; 1. Check argc
    call $get_arg_count
    local.set $argc

    (i32.gt_s (local.get $argc) (i32.const 1))
    if
      ;; Retrieve argument to offset 1024
      (call $get_arg (i32.const 1) (i32.const 1024) (i32.const 1000))
      local.set $arg_len

      ;; Check if we got the argument successfully
      (i32.le_s (local.get $arg_len) (i32.const 0))
      if
        i32.const -1
        return
      end

      ;; Open the file
      (call $open (i32.const 1024))
      local.set $fd
    else
      ;; No argument, read from stdin (fd 0)
      i32.const 0
      local.set $fd
    end

    ;; Check if fd is valid (< 0 means error)
    (i32.lt_s (local.get $fd) (i32.const 0))
    if
      ;; Write error message: "Unable to open file\n"
      ;; Prepare __wasi_ciovec_t at 3072
      (i32.store (i32.const 3072) (i32.const 3100)) ;; Pointer to msg
      (i32.store (i32.const 3076) (i32.const 20))   ;; Length of msg
      
      (call $write
        (i32.const 1)    ;; stdout
        (i32.const 3072) ;; iovs
        (i32.const 1)    ;; iovs_len
        (i32.const 3088) ;; nwritten
      )
      drop
      i32.const -1
      return
    end

    ;; 2. Read loop
    block $read_loop_end
      loop $read_loop
        ;; Read from fd into offset 2048
        (call $read
          (local.get $fd)
          (i32.const 2048) ;; Buffer offset
          (i32.const 1024) ;; Read up to 1024 bytes
        )
        local.set $bytes_read

        ;; If bytes_read <= 0, we are done
        (i32.le_s (local.get $bytes_read) (i32.const 0))
        if
          br $read_loop_end
        end

        ;; Prepare __wasi_ciovec_t at 3072
        (i32.store (i32.const 3072) (i32.const 2048)) ;; Pointer to read buffer
        (i32.store (i32.const 3076) (local.get $bytes_read)) ;; Length of buffer

        ;; Write to stdout (fd 1)
        (call $write
          (i32.const 1)    ;; stdout
          (i32.const 3072) ;; iovs
          (i32.const 1)    ;; iovs_len
          (i32.const 3088) ;; nwritten
        )
        drop

        br $read_loop
      end
    end

    ;; 3. Close the file if it's not stdin
    (i32.ne (local.get $fd) (i32.const 0))
    if
      (call $close (local.get $fd))
      drop
    end

    i32.const 0
  )
)
