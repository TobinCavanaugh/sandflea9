(module
  ;; WASI imports (linked by our kernel-native kern_link_wasi)
  (import "wasi_snapshot_preview1" "fd_write" (func $fd_write (param i32 i32 i32 i32) (result i32)))

  (memory 1)

  ;; "Hello, World!\n" at offset 16
  (data (i32.const 16) "Hello, World!\n")

  ;; __wasi_ciovec_t at offset 0: { buf = 16, buf_len = 14 }
  (data (i32.const 0) "\10\00\00\00")  ;; buf ptr = 16 (little-endian u32)
  (data (i32.const 4) "\0e\00\00\00")  ;; buf_len = 14 (little-endian u32)

  ;; nwritten at offset 8
  (data (i32.const 8) "\00\00\00\00")

  (func (export "_start")
    (call $fd_write
      (i32.const 1)     ;; fd = stdout
      (i32.const 0)     ;; iovs = pointer to iovec at offset 0
      (i32.const 1)     ;; iovs_len = 1
      (i32.const 8)     ;; nwritten = pointer at offset 8
    )
    drop
  )
)
