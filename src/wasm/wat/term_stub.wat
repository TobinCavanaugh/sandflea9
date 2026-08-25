;; term_stub.wat — mini terminal app for the compositing WM.
;;
;; Input flow:
;;   WM creates a 1-page shm ring, passes shm_id as argv[0] (env.get_arg_i32).
;;   WM writes key bytes into the ring (offset 2 + head%256), publishes a
;;   tail byte counter (offset 1), then signals SIG_KEY (bit 8).
;;   We drain the ring on each SIG_KEY:  head counter at offset 0,
;;   ring bytes at 2..257.  SIG_CLOSE (bit 4) exits the process.
;;
;; Renders typed characters to its framebuffer (blitted by the WM), with a
;; "> " prompt per line; Enter breaks the line, Backspace erases.

(module
  (import "display" "claimBuffer" (func $claimBuffer (result i32)))
  (import "display" "getResolution" (func $getResolution (result i32)))
  (import "display" "present" (func $present (param i32) (result i32)))
  (import "env" "ipc_signal_wait" (func $ipc_signal_wait (param i32) (result i32)))
  (import "env" "get_arg_i32" (func $get_arg_i32 (param i32) (result i32)))
  (import "env" "ipc_shm_attach" (func $ipc_shm_attach (param i32) (result i32)))
  (import "env" "ipc_shm_read_byte" (func $ipc_shm_read_byte (param i32 i32) (result i32)))
  (import "env" "ipc_shm_write_byte" (func $ipc_shm_write_byte (param i32 i32 i32) (result i32)))

  (memory 1)

  ;; ── 5x7 font, 95 glyphs (ASCII 0x20..0x7E), 7 bytes per glyph ──────────
  ;; glyph index = (ch - 0x20) * 7
  (data (i32.const 0)
    "\00\00\00\00\00\00\00"  ;; 0x20 space
    "\04\04\04\04\00\00\04"  ;; 0x21 !
    "\0a\0a\0a\00\00\00\00"  ;; 0x22 "
    "\0a\0a\1f\0a\1f\0a\0a"  ;; 0x23 #
    "\04\0e\05\0e\14\0e\04"  ;; 0x24 $
    "\19\19\02\04\08\13\13"  ;; 0x25 %
    "\0c\12\14\08\15\12\0d"  ;; 0x26 &
    "\04\04\04\00\00\00\00"  ;; 0x27 '
    "\02\04\08\08\08\04\02"  ;; 0x28 (
    "\08\04\02\02\02\04\08"  ;; 0x29 )
    "\00\0a\04\1f\04\0a\00"  ;; 0x2a *
    "\00\04\04\1f\04\04\00"  ;; 0x2b +
    "\00\00\00\00\02\04\08"  ;; 0x2c ,
    "\00\00\00\1f\00\00\00"  ;; 0x2d -
    "\00\00\00\00\00\0c\0c"  ;; 0x2e .
    "\01\02\04\08\10\00\00"  ;; 0x2f /
    "\0e\11\13\15\19\11\0e"  ;; 0x30 0
    "\04\0c\04\04\04\04\0e"  ;; 0x31 1
    "\0e\11\01\02\04\08\1f"  ;; 0x32 2
    "\1f\02\04\02\01\11\0e"  ;; 0x33 3
    "\02\06\0a\12\1f\02\02"  ;; 0x34 4
    "\1f\10\1e\01\01\11\0e"  ;; 0x35 5
    "\06\08\10\1e\11\11\0e"  ;; 0x36 6
    "\1f\01\02\04\08\08\08"  ;; 0x37 7
    "\0e\11\11\0e\11\11\0e"  ;; 0x38 8
    "\0e\11\11\0f\01\02\0c"  ;; 0x39 9
    "\00\0c\0c\00\0c\0c\00"  ;; 0x3a :
    "\00\04\04\00\04\04\08"  ;; 0x3b ;
    "\02\04\08\10\08\04\02"  ;; 0x3c <
    "\00\00\1f\00\1f\00\00"  ;; 0x3d =
    "\08\04\02\01\02\04\08"  ;; 0x3e >
    "\0e\11\01\02\04\00\04"  ;; 0x3f ?
    "\0e\11\01\0d\15\15\0e"  ;; 0x40 @
    "\0e\11\11\1f\11\11\11"  ;; 0x41 A
    "\1e\11\11\1e\11\11\1e"  ;; 0x42 B
    "\0e\11\10\10\10\11\0e"  ;; 0x43 C
    "\1e\11\11\11\11\11\1e"  ;; 0x44 D
    "\1f\10\10\1e\10\10\1f"  ;; 0x45 E
    "\1f\10\10\1e\10\10\10"  ;; 0x46 F
    "\0e\11\10\17\11\11\0f"  ;; 0x47 G
    "\11\11\11\1f\11\11\11"  ;; 0x48 H
    "\0e\04\04\04\04\04\0e"  ;; 0x49 I
    "\07\02\02\02\02\12\0c"  ;; 0x4a J
    "\11\12\14\18\14\12\11"  ;; 0x4b K
    "\10\10\10\10\10\10\1f"  ;; 0x4c L
    "\11\1b\15\15\11\11\11"  ;; 0x4d M
    "\11\19\15\13\11\11\11"  ;; 0x4e N
    "\0e\11\11\11\11\11\0e"  ;; 0x4f O
    "\1e\11\11\1e\10\10\10"  ;; 0x50 P
    "\0e\11\11\11\15\12\0d"  ;; 0x51 Q
    "\1e\11\11\1e\14\12\11"  ;; 0x52 R
    "\0e\11\10\0e\01\11\0e"  ;; 0x53 S
    "\1f\04\04\04\04\04\04"  ;; 0x54 T
    "\11\11\11\11\11\11\0e"  ;; 0x55 U
    "\11\11\11\11\11\0a\04"  ;; 0x56 V
    "\11\11\11\15\15\1b\11"  ;; 0x57 W
    "\11\11\0a\04\0a\11\11"  ;; 0x58 X
    "\11\11\0a\04\04\04\04"  ;; 0x59 Y
    "\1f\01\02\04\08\10\1f"  ;; 0x5a Z
    "\0e\08\08\08\08\08\0e"  ;; 0x5b [
    "\10\08\04\02\01\00\00"  ;; 0x5c backslash
    "\0e\02\02\02\02\02\0e"  ;; 0x5d ]
    "\04\0a\11\00\00\00\00"  ;; 0x5e ^
    "\00\00\00\00\00\00\1f"  ;; 0x5f _
    "\08\04\02\00\00\00\00"  ;; 0x60 `
    "\00\00\0e\01\0f\11\0f"  ;; 0x61 a
    "\10\10\16\19\11\11\1e"  ;; 0x62 b
    "\00\00\0e\11\10\11\0e"  ;; 0x63 c
    "\01\01\0d\13\11\11\0f"  ;; 0x64 d
    "\00\00\0e\11\1f\10\0e"  ;; 0x65 e
    "\06\09\08\1c\08\08\08"  ;; 0x66 f
    "\00\0f\11\11\0f\01\0e"  ;; 0x67 g
    "\10\10\16\19\11\11\11"  ;; 0x68 h
    "\04\00\0c\04\04\04\0e"  ;; 0x69 i
    "\02\00\06\02\02\12\0c"  ;; 0x6a j
    "\10\10\12\14\18\14\12"  ;; 0x6b k
    "\0c\04\04\04\04\04\0e"  ;; 0x6c l
    "\00\00\1a\15\15\15\15"  ;; 0x6d m
    "\00\00\16\19\11\11\11"  ;; 0x6e n
    "\00\00\0e\11\11\11\0e"  ;; 0x6f o
    "\00\1e\11\11\1e\10\10"  ;; 0x70 p
    "\00\0d\13\11\0f\01\01"  ;; 0x71 q
    "\00\00\16\19\10\10\10"  ;; 0x72 r
    "\00\00\0f\10\0e\01\1e"  ;; 0x73 s
    "\08\08\1c\08\08\09\06"  ;; 0x74 t
    "\00\00\11\11\11\13\0d"  ;; 0x75 u
    "\00\00\11\11\11\0a\04"  ;; 0x76 v
    "\00\00\11\11\15\15\0a"  ;; 0x77 w
    "\00\00\11\0a\04\0a\11"  ;; 0x78 x
    "\00\11\11\11\0f\01\0e"  ;; 0x79 y
    "\00\00\1f\02\04\08\1f"  ;; 0x7a z
    "\06\08\08\10\08\08\06"  ;; 0x7b {
    "\04\04\04\04\04\04\04"  ;; 0x7c |
    "\0c\02\02\01\02\02\0c"  ;; 0x7d }
    "\08\15\02\00\00\00\00"  ;; 0x7e ~
  )

  ;; Colors: dark terminal background, bright green text.
  ;; Geometry: 8px margin, 6px char pitch (5 wide + 1), 8px line height (7 tall + 1).

  ;; ── Fill the whole framebuffer with the background color ────────────────
  (func $fill_bg (param $buf i32) (param $stride i32) (param $h i32)
    (local $w i32) (local $y i32) (local $x i32) (local $addr i32)
    (local.set $w (i32.div_s (local.get $stride) (i32.const 4)))
    (local.set $y (i32.const 0))
    (block $done_y
      (loop $row_loop
        (br_if $done_y (i32.ge_s (local.get $y) (local.get $h)))
        (local.set $x (i32.const 0))
        (block $done_x
          (loop $col_loop
            (br_if $done_x (i32.ge_s (local.get $x) (local.get $w)))
            (local.set $addr
              (i32.add
                (i32.add (local.get $buf)
                  (i32.mul (local.get $y) (local.get $stride)))
                (i32.mul (local.get $x) (i32.const 4))))
            (i32.store (local.get $addr) (i32.const 0xFF101018))
            (local.set $x (i32.add (local.get $x) (i32.const 1)))
            (br $col_loop)))
        (local.set $y (i32.add (local.get $y) (i32.const 1)))
        (br $row_loop)))
  )

  ;; ── Draw one ASCII char at character position (col, row) ────────────────
  (func $draw_char (param $buf i32) (param $stride i32)
                   (param $ch i32) (param $col i32) (param $row i32)
    (local $g i32) (local $r i32) (local $c i32) (local $bits i32) (local $addr i32)
    (local.set $g (i32.mul (i32.sub (local.get $ch) (i32.const 0x20)) (i32.const 7)))
    (local.set $r (i32.const 0))
    (block $done_r
      (loop $row_loop
        (br_if $done_r (i32.ge_s (local.get $r) (i32.const 7)))
        (local.set $bits (i32.load8_u (i32.add (local.get $g) (local.get $r))))
        (local.set $c (i32.const 0))
        (block $done_c
          (loop $col_loop
            (br_if $done_c (i32.ge_s (local.get $c) (i32.const 5)))
            (i32.and
              (i32.shr_u (local.get $bits) (i32.sub (i32.const 4) (local.get $c)))
              (i32.const 1))
            (if
              (then
                (local.set $addr
                  (i32.add
                    (i32.add
                      (local.get $buf)
                      (i32.mul
                        (i32.add (i32.const 8)
                          (i32.add (i32.mul (local.get $row) (i32.const 8)) (local.get $r)))
                        (local.get $stride)))
                    (i32.mul
                      (i32.add (i32.const 8)
                        (i32.add (i32.mul (local.get $col) (i32.const 6)) (local.get $c)))
                      (i32.const 4))))
                (i32.store (local.get $addr) (i32.const 0xFF55FF55))))
            (local.set $c (i32.add (local.get $c) (i32.const 1)))
            (br $col_loop)))
        (local.set $r (i32.add (local.get $r) (i32.const 1)))
        (br $row_loop)))
  )

  ;; ── Erase one character cell (6x8) back to background ───────────────────
  (func $erase_cell (param $buf i32) (param $stride i32) (param $col i32) (param $row i32)
    (local $r i32) (local $c i32) (local $addr i32)
    (local.set $r (i32.const 0))
    (block $done_r
      (loop $row_loop
        (br_if $done_r (i32.ge_s (local.get $r) (i32.const 8)))
        (local.set $c (i32.const 0))
        (block $done_c
          (loop $col_loop
            (br_if $done_c (i32.ge_s (local.get $c) (i32.const 6)))
            (local.set $addr
              (i32.add
                (i32.add
                  (local.get $buf)
                  (i32.mul
                    (i32.add (i32.const 8)
                      (i32.add (i32.mul (local.get $row) (i32.const 8)) (local.get $r)))
                    (local.get $stride)))
                (i32.mul
                  (i32.add (i32.const 8)
                    (i32.add (i32.mul (local.get $col) (i32.const 6)) (local.get $c)))
                  (i32.const 4))))
            (i32.store (local.get $addr) (i32.const 0xFF101018))
            (local.set $c (i32.add (local.get $c) (i32.const 1)))
            (br $col_loop)))
        (local.set $r (i32.add (local.get $r) (i32.const 1)))
        (br $row_loop)))
  )

  ;; ── Draw the "> " prompt at the start of a line ─────────────────────────
  (func $draw_prompt (param $buf i32) (param $stride i32) (param $row i32)
    (call $draw_char (local.get $buf) (local.get $stride) (i32.const 0x3E) (i32.const 0) (local.get $row))
    (call $draw_char (local.get $buf) (local.get $stride) (i32.const 0x20) (i32.const 1) (local.get $row))
  )

  ;; ── _start ──────────────────────────────────────────────────────────────
  (func (export "_start")
    (local $buf i32) (local $stride i32) (local $res i32)
    (local $w i32) (local $h i32)
    (local $shm_id i32) (local $handle i32)
    (local $max_cols i32) (local $max_rows i32)
    (local $col i32) (local $row i32)
    (local $sig i32) (local $head i32) (local $tail i32) (local $ch i32)

    ;; 1. Claim the display buffer (kernel grows memory to screen size)
    (call $claimBuffer)
    (local.set $buf)
    (i32.lt_s (local.get $buf) (i32.const 0))
    (if (then (return)))

    ;; 2. Resolution → width / height / stride
    (call $getResolution)
    (local.set $res)
    (local.set $w (i32.and (local.get $res) (i32.const 0xFFFF)))
    (local.set $h (i32.shr_u (local.get $res) (i32.const 16)))
    (local.set $stride (i32.mul (local.get $w) (i32.const 4)))

    ;; 3. Fill background once
    (call $fill_bg (local.get $buf) (local.get $stride) (local.get $h))

    ;; 4. Attach to the WM-provided input ring (argv[0] = shm_id)
    (local.set $handle (i32.const -1))
    (call $get_arg_i32 (i32.const 0))
    (local.set $shm_id)
    (i32.gt_s (local.get $shm_id) (i32.const 0))
    (if (then
      (call $ipc_shm_attach (local.get $shm_id))
      (local.set $handle)))

    ;; 5. Terminal geometry — assume the window is at least half the screen
    (local.set $max_cols
      (i32.div_s (i32.sub (i32.div_s (local.get $w) (i32.const 2)) (i32.const 16)) (i32.const 6)))
    (i32.lt_s (local.get $max_cols) (i32.const 8))
    (if (then (local.set $max_cols (i32.const 8))))
    (i32.gt_s (local.get $max_cols) (i32.const 40))
    (if (then (local.set $max_cols (i32.const 40))))
    (local.set $max_rows
      (i32.div_s (i32.sub (i32.div_s (local.get $h) (i32.const 2)) (i32.const 16)) (i32.const 8)))
    (i32.lt_s (local.get $max_rows) (i32.const 4))
    (if (then (local.set $max_rows (i32.const 4))))
    (i32.gt_s (local.get $max_rows) (i32.const 30))
    (if (then (local.set $max_rows (i32.const 30))))

    ;; 6. Start at row 0 with a prompt
    (local.set $col (i32.const 2))
    (local.set $row (i32.const 0))
    (call $draw_prompt (local.get $buf) (local.get $stride) (local.get $row))

    ;; 7. Event loop: wait for signals, drain the key ring
    (block $done
      (loop $again
        (call $ipc_signal_wait (i32.const 0xFFFF))
        (local.set $sig)

        ;; SIG_CLOSE (bit 4): exit the process
        (i32.and (local.get $sig) (i32.const 4))
        (if (then (br $done)))

        ;; SIG_KEY (bit 8): drain ring bytes
        (i32.and (local.get $sig) (i32.const 8))
        (if
          (then
            (i32.ge_s (local.get $handle) (i32.const 0))
            (if
              (then
                (call $ipc_shm_read_byte (local.get $handle) (i32.const 0))
                (local.set $head)
                (call $ipc_shm_read_byte (local.get $handle) (i32.const 1))
                (local.set $tail)
                (block $done_drain
                  (loop $drain
                    (br_if $done_drain (i32.eq (local.get $head) (local.get $tail)))
                    (call $ipc_shm_read_byte (local.get $handle)
                      (i32.add (i32.const 2) (local.get $head)))
                    (local.set $ch)

                    ;; ── newline: next row + fresh prompt ──
                    (i32.or
                      (i32.eq (local.get $ch) (i32.const 0x0A))
                      (i32.eq (local.get $ch) (i32.const 0x0D)))
                    (if (then
                      (i32.lt_s (local.get $row) (i32.sub (local.get $max_rows) (i32.const 1)))
                      (if (then (local.set $row (i32.add (local.get $row) (i32.const 1)))))
                      (local.set $col (i32.const 2))
                      (call $draw_prompt (local.get $buf) (local.get $stride) (local.get $row))))

                    ;; ── backspace: erase previous char (keep the prompt) ──
                    (i32.eq (local.get $ch) (i32.const 0x08))
                    (if (then
                      (i32.gt_s (local.get $col) (i32.const 2))
                      (if (then
                        (local.set $col (i32.sub (local.get $col) (i32.const 1)))
                        (call $erase_cell (local.get $buf) (local.get $stride) (local.get $col) (local.get $row))))))

                    ;; ── printable: wrap at end of line, then draw ──
                    (i32.and
                      (i32.ge_s (local.get $ch) (i32.const 0x20))
                      (i32.le_s (local.get $ch) (i32.const 0x7E)))
                    (if (then
                      (i32.ge_s (local.get $col) (local.get $max_cols))
                      (if (then
                        (i32.lt_s (local.get $row) (i32.sub (local.get $max_rows) (i32.const 1)))
                        (if (then (local.set $row (i32.add (local.get $row) (i32.const 1)))))
                        (local.set $col (i32.const 2))
                        (call $draw_prompt (local.get $buf) (local.get $stride) (local.get $row))))
                      (call $draw_char (local.get $buf) (local.get $stride)
                        (local.get $ch) (local.get $col) (local.get $row))
                      (local.set $col (i32.add (local.get $col) (i32.const 1)))))

                    ;; advance head
                    (local.set $head (i32.and (i32.add (local.get $head) (i32.const 1)) (i32.const 0xFF)))
                    (call $ipc_shm_write_byte (local.get $handle) (i32.const 0) (local.get $head))
                    (br $drain)))
              ))
            ;; Redraw signal (no-op under a compositor, harmless otherwise)
            (call $present (local.get $buf))
            (drop)
          ))
        (br $again)))
  )
)
