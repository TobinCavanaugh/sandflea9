(module
  ;; Imports from sandfleaOS kernel
  (import "env" "lsr" (func $lsr (param i32) (result i32)))
  (import "env" "get_arg_count" (func $get_arg_count (result i32)))
  (import "env" "get_arg" (func $get_arg (param i32 i32 i32) (result i32)))

  ;; Linear memory (1 page = 64KB)
  (memory 1)

  ;; Memory layout:
  ;; Offset 1024: Buffer to store command line argument (path)
  ;; Default path is "/"
  (data (i32.const 1024) "/\00")

  (func (export "_start") (result i32)
    (local $argc i32)
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
    end

    ;; Call the lsr host function with path at 1024
    (call $lsr (i32.const 1024))
  )
)
