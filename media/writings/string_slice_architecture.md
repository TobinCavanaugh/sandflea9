# String Slices for sandfleaOS

**Status:** Design Proposal  
**Date:** July 2026  
**Updated:** July 2026 — renamed types for clarity and C interop safety

---

## 1. Problem Statement

### 1.1 Current Pain Points

| Problem | Example | Cost |
|---------|---------|------|
| **Accidental O(n) strlen** | `str_eq(a, b)` calls `str_len(a)` + `str_len(b)` — two full scans just to compare | Performance |
| **Forced null-termination** | `str_dup_len(src, len, kmalloc)` copies `len` bytes then appends `\0` — extra byte, extra write | Memory |
| **No substrings without copying** | Want to extract "hello" from `"GET /hello HTTP/1.1"`? Must copy + null-terminate | Complexity |
| **Binary data can't be passed** | If `\0` appears in data, strlen truncates, strcmp stops early — unusable | Bugs |
| **Double-scan everywhere** | `cmd_word_t` stores `loc` + `len` (a slice!), but `cmd_word_eq` calls `str_len(match)` on the literal, then `str_eql(word->loc, match, mlen)` — we already know `word->len` but it's not always used | Waste |
| **File paths** | `ext2_find_path` manually tokenizes with a 256-byte stack buffer, rebuilding each component | Awkward |
| **Command parsing copies** | 15 `str_dup`/`str_dup_len` calls in `kern_tests.c` alone, all for data that already lives in `cmd_word_t.loc` + `cmd_word_t.len` | Churn |

### 1.2 Why Slices?

A **string view** is a non-owning reference: `{ pointer, length }`. No null terminator needed. No scanning. Substrings are free. Comparison is `memcmp`. This is how Rust (`&str`), Go (`[]byte` string conversion), and Zig (`[]const u8`) handle strings. In a kernel:

- **Paths are parsed into components** without copying
- **Command parsing** already produces slices (`cmd_word_t` is one!)
- **WASM linear memory** has explicit lengths — converting to null-terminated is wasteful
- **Comparison is O(1) len check + memcmp** — not double strlen + compare
- **Binary-safe** — handles `\0` in data, important for file contents, WASM modules, etc.

---

## 2. Core Types

### 2.1 `str_view_t` — Non-Owning Slice (like `&str` / `std::string_view`)

```c
typedef struct {
    const char *data;   // pointer into existing memory (NOT owned)
    u32         len;    // byte length, NOT including any null terminator
} str_view_t;
```

- **Does not own** the memory it points to
- **Not null-terminated** — may point into the middle of a larger buffer
- **Lifetime**: valid only as long as the underlying buffer exists
- **Size**: 12 bytes on x86_64 (pointer + u32), fits in registers
- **Construction from `str_t`**: `str_view(s)` — always valid since `str_t` null-terminates, so `data[len] == '\0'` even though the view doesn't "own" the terminator

### 2.2 `str_t` — Owning String (like `String` / `std::string`)

```c
typedef struct {
    char *data;     // heap-allocated (kmalloc), **always null-terminated**
    u32   len;      // byte length (not counting \0)
    u32   cap;      // allocated capacity (including \0 byte)
} str_t;
```

- **Owns** the heap allocation — `str_free()` frees it
- **Always null-terminated** — `data[len] == '\0'` by invariant. `.data` can be passed directly to any C function expecting `const char *` (printf, screen_puts, fopen, etc.). This is the key interop guarantee.
- **Growable** — `str_append()`, `str_push()`, etc.
- **Allocation**: `kmalloc(cap)` where `cap >= len + 1`
- **Size**: 16 bytes on x86_64 (pointer + u32 + u32)

### 2.3 `STR_VIEW_LIT` — Compile-Time View Literals

```c
// Macro for string literals — evaluated at compile time, produces str_view_t
#define STR_VIEW_LIT(c_string_literal) \
    (str_view_t){ .data = (c_string_literal), .len = sizeof(c_string_literal) - 1 }
```

Zero runtime cost.

```c
str_view_t name = STR_VIEW_LIT("hello");  // → { .data = "hello", .len = 5 }
```

### 2.4 Relationship Diagram

```
str_view_t  ──references──▶  str_t
 (non-owning)                (owning, \0-terminated)
 {data, len}                 {data, len, cap}
 12 bytes                    16 bytes

str_view_t  ──references──▶  const char * (C string literal)
                              (lives in .rodata, forever)

str_view_t  ──references──▶  cmd_word_t.loc + .len
                              (lives in the input buffer)
```

---

## 3. API Design

### 3.1 Construction

```c
// === str_view_t construction ===

// From a null-terminated C string (scans to find length — use sparingly)
str_view_t str_view_from_c(const char *s);

// From a pointer + length (no scan — preferred)
str_view_t str_view_from_parts(const char *data, u32 len);

// From a str_t (borrows — valid as long as the str_t lives)
str_view_t str_view_from_str(const str_t *s);

// Macro shorthand for the above
#define str_view(s) str_view_from_str(s)


// === str_t construction ===

// Create empty string with initial capacity (cap includes \0 byte, so min cap is 1)
str_t str_new(u32 cap);

// Create by copying a str_view_t (always null-terminates)
str_t str_from_view(str_view_t sv);

// Create by copying a null-terminated C string
str_t str_from_c(const char *s);

// Free a str_t
void str_free(str_t *s);
```

### 3.2 Query

```c
// === str_view_t query (all O(1)) ===

u32  str_view_len(str_view_t sv);          // → sv.len
bool str_view_is_empty(str_view_t sv);     // → sv.len == 0
char str_view_at(str_view_t sv, u32 i);    // → sv.data[i] (bounds-checked in debug)


// === str_t query (all O(1)) ===

u32  str_len(const str_t *s);              // → s->len
u32  str_cap(const str_t *s);              // → s->cap
bool str_is_empty(const str_t *s);         // → s->len == 0
char str_at(const str_t *s, u32 i);        // → s->data[i] (bounds-checked in debug)

// Borrow as a view — the primary way to pass a str_t to functions
str_view_t str_view_from_str(const str_t *s);  // → {s->data, s->len}
```

### 3.3 Comparison

All O(1) length check first, then O(min(len_a, len_b)) memcmp.

```c
// === str_view_t comparison ===

bool str_view_eq(str_view_t a, str_view_t b);            // exact equality
bool str_view_starts_with(str_view_t sv, str_view_t prefix);
bool str_view_ends_with(str_view_t sv, str_view_t suffix);
i32  str_view_cmp(str_view_t a, str_view_t b);           // -1, 0, 1


// === str_t convenience wrappers ===
// (These just borrow as str_view_t and delegate)

bool str_eq_str(const str_t *a, const str_t *b);
bool str_eq_view(const str_t *s, str_view_t sv);
```

### 3.4 Slicing (Zero-Copy on str_view_t)

```c
// Substring from offset to end
str_view_t str_view_slice_from(str_view_t sv, u32 start);

// Substring from start for len bytes
str_view_t str_view_slice(str_view_t sv, u32 start, u32 len);

// Split on first occurrence of delimiter
// Returns { before, after } — both slices, no allocation
typedef struct { str_view_t left; str_view_t right; } str_view_split_t;
str_view_split_t str_view_split_once(str_view_t sv, char delimiter);

// Split into components (for path parsing etc.)
// Returns the number of components found (max N)
u32 str_view_split(str_view_t sv, char delimiter, str_view_t *out, u32 max_out);

// Trim leading/trailing whitespace
str_view_t str_view_trim(str_view_t sv);

// Find character, returns offset or -1
i32 str_view_find(str_view_t sv, char c);
i32 str_view_find_last(str_view_t sv, char c);
```

**Example — path parsing without copies:**

```c
str_view_t path = STR_VIEW_LIT("/folder/subdir/file.txt");

// Split: ["folder", "subdir", "file.txt"]
str_view_t parts[8];
u32 n = str_view_split(str_view_slice_from(path, 1), '/', parts, 8);
// parts[0] = {"folder", 6}, parts[1] = {"subdir", 6}, parts[2] = {"file.txt", 8}
// No kmalloc, no stack buffers, no null terminators
```

### 3.5 Conversion

```c
// === str_view_t → C string (when null-termination is REQUIRED) ===

// Allocates a null-terminated copy on the heap (caller must kfree)
char *str_view_to_c(str_view_t sv);

// Writes into a pre-allocated buffer (returns number of bytes written, excluding \0)
u32 str_view_to_buf(str_view_t sv, char *out, u32 out_cap);


// === str_t → C string (FREE — just read .data) ===

// str_t.data is ALWAYS null-terminated. Use directly:
//   screen_push_line(s->data);
//   printf("%s", s->data);
//   fs_open(s->data);
// No conversion needed. No scanning. No allocation.
```

### 3.6 `str_t` Mutation API

```c
// Append a str_view_t to a str_t (grows if needed)
void str_append(str_t *s, str_view_t sv);

// Append a single character
void str_push(str_t *s, char c);

// Clear contents (keep allocation, len → 0, data[0] = '\0')
void str_clear(str_t *s);

// Ensure capacity for at least new_cap bytes (including \0)
void str_reserve(str_t *s, u32 new_cap);

// Shrink capacity to exactly len+1
void str_shrink(str_t *s);

// Set from a str_view_t (replaces contents, grows if needed)
void str_set(str_t *s, str_view_t sv);
```

---

## 4. Key Design Decisions

### 4.1 `str_t` Is Always Null-Terminated

This is the central interop guarantee. Every `str_t` allocates `cap >= len + 1` bytes and maintains `data[len] == '\0'`. This means:

- **`.data` is a valid C string** — pass it to `printf`, `screen_push_line`, `fs_open`, any function expecting `const char *`
- **No conversion cost for C interop** — unlike `str_view_t` which requires `str_view_to_c()` (alloc + copy)
- **Substrings of a `str_t` via `str_view_from_str()`** — the view's `data[len]` is the parent's null terminator, but the view doesn't "own" it — it just happens to be there

This avoids the Rust problem where you constantly convert between `&str` and `CString` for FFI. In a kernel, C interop is the default, not the exception.

### 4.2 When to Use Each Type

| Context | Use | Reason |
|---------|-----|--------|
| **Storing a string** (global, struct field, return value) | `str_t` | Owns memory, null-terminated |
| **Passing a string to a function** (parameter) | `str_view_t` | Borrows, no allocation, works with `str_t` and C literals |
| **Command parsing** | `str_view_t` | `cmd_word_t` already stores `{loc, len}` — slices are free |
| **WASM linear memory reads** | `str_view_t` | WASM has explicit lengths, null termination is wasted work |
| **Path tokenization** | `str_view_t` | Split into components without copying |
| **C interop** (printf, fs_open, screen_puts) | `str_t` or `str_t.data` | `.data` is always null-terminated — just pass it |
| **String building** (format, concatenate) | `str_t` | Growable, null-terminated at every step |

### 4.3 Why `.len` Instead of Null-Terminated (for str_view_t)

1. **O(1) length**: No scanning. `str_view_len(sv)` reads a field.
2. **Binary-safe**: A filename containing `\0` won't silently truncate (rare but possible in ext2, which uses length-prefixed names).
3. **Substrings are free**: `str_view_slice_from(sv, 5)` adjusts `.data += 5, .len -= 5`. No allocation.
4. **Always know the bounds**: `str_view_at(sv, i)` can bounds-check in debug builds — no reading past the end.

### 4.4 Why Keep `.data` as `const char *` (Not `u8 *`)

- Most string operations work on text (`char` is more readable)
- Binary data can use a separate `bytes_t` type if needed
- C string literals are `char *` — avoids casts everywhere
- The kernel doesn't do UTF-8 manipulation (yet); just passes bytes through

### 4.5 Ownership Model

```
  str_view_t   → borrows, never owns        (like &str / string_view)
  str_t        → owns, always null-terminated (like String / std::string)
  const char * → legacy, null-terminated, no length
```

No implicit conversions between them (no surprises). Explicit:

```c
// Borrow from str_t:
str_view_t sv = str_view_from_str(&s);    // s must outlive sv

// Borrow from C literal:
str_view_t sv = STR_VIEW_LIT("hello");    // lives forever (.rodata)

// Own from view:
str_t s = str_from_view(sv);              // copies + null-terminates

// Pass str_t to C function:
screen_push_line(s.data);                 // always safe, .data is \0-terminated

// Pass str_view_t to C function:
char *tmp = str_view_to_c(sv);            // allocate + null-terminate
screen_push_line(tmp);
kfree(tmp);
```

---

## 5. Migration Strategy

### 5.1 Phase 1: Add the Types and Basic API (Next)

1. Create `src/util/str_slice.h` with `str_t`, `str_view_t`, `STR_VIEW_LIT`, and core functions
2. Create `src/util/str_slice.c` with implementations
3. Add to `build.sh` C_SOURCES
4. **Don't change any existing code** — the old `str_*` functions keep working

### 5.2 Phase 2: Convert `cmd_word_t` to Use `str_view_t`

`cmd_word_t` already stores `loc` and `len` — it's already a view! Just add convenience:

```c
// Old:
u8 cmd_word_eq(cmd_word_t *word, const char *match);

// New: direct str_view_t comparison, no strlen on the literal
#define cmd_word_eq(word, literal) \
    str_view_eq(cmd_word_view(word), STR_VIEW_LIT(literal))

// Helper:
str_view_t cmd_word_view(const cmd_word_t *w) {
    return str_view_from_parts(w->loc, w->len);
}
```

### 5.3 Phase 3: Convert File Path APIs

```c
// Old:
i32 fs_open(const char *path);

// New overload:
i32 fs_open_view(str_view_t path);

// Convenience wrapper for C strings (backward compat):
static inline i32 fs_open(const char *cstr) {
    return fs_open_view(str_view_from_c(cstr));
}
```

`ext2_find_path` currently tokenizes paths by scanning for `/` into a 256-byte stack buffer. With views:

```c
// Old: ~40 lines of manual tokenization
// New:
str_view_t components[16];
u32 n = str_view_split(str_view_slice_from(path, 1), '/', components, 16);
for (u32 i = 0; i < n; i++) {
    current_inode = ext2_find_child(current_inode, components[i]);
}
```

### 5.4 Phase 4: Convert Terminal and Screen Output

`term_write` and `screen_push_buf` already take `(buf, len)` — they're view-ready. Add view overloads:

```c
void term_write_view(str_view_t sv);
void screen_push_view(str_view_t sv);
```

Wrap old functions:
```c
void screen_push_line(const char *cstr) {
    screen_push_view(str_view_from_c(cstr));
}
```

### 5.5 Phase 5: Convert WASM FFI

WASM host functions receive pointers into linear memory with explicit sizes. Currently:

```c
// wasm_spawn.c — the host function gets a pointer but no length info
m3ApiRawFunction(wasm_get_arg) {
    m3ApiGetArg(u32, index)
    // ... reads from wasm memory, hard to know length
}
```

With views:

```c
str_view_t wasm_read_str(u8 *wasm_mem, u32 offset, u32 len) {
    return str_view_from_parts((const char *)(wasm_mem + offset), len);
}
```

No null-termination needed to pass from WASM → kernel. For WASM → kernel → WASM round-trips: just pass the `(offset, len)` pair.

---

## 6. What We Keep

Not everything changes. These stay as-is:

- **`stbsupport.c`**: `printf`, `snprintf`, `puts` — these match POSIX signatures for libc compatibility and are only used for formatting, not general string handling.
- **`str_len`, `str_dup`, `str_eql`** (old functions): Keep the old functions (they're already used everywhere). They can be reimplemented in terms of `str_view_t` internally. Eventually deprecate.
- **`fmt` functions**: `stbsp_snprintf`, `serial_outsf`, `screen_push_linef` — these format strings at runtime; views don't help here (format strings are always C strings).

---

## 7. Example: `wat2wasm` Command Handler (Before/After)

### Before (Current)

```c
if (cmd_word_eq(word, "wat2wasm")) {
    char *input = null, *output = null;
    if (word->next != null) {
        input = str_dup_len(word->next->loc, word->next->len, kmalloc);
    }
    cmd_word_t *out_word = word->next ? word->next->next : null;
    if (out_word != null && cmd_word_eq(out_word, "-o")) {
        out_word = out_word->next;
    }
    if (out_word != null) {
        output = str_dup_len(out_word->loc, out_word->len, kmalloc);
    }
    if (!input || !output) {
        screen_push_line("Usage: wat2wasm <input.wat> <output.wasm>");
        if (input) kfree(input);
        if (output) kfree(output);
        goto Label_Free;
    }
    screen_push_linef("wat2wasm: compiling %s -> %s", input, output);
    char *argv_native[4] = { "wat2wasm", input, "-o", output };
    wat2wasm_native(4, argv_native);
    kfree(input);
    kfree(output);
    goto Label_Free;
}
```

### After (With Views + str_t)

```c
if (str_view_eq(cmd_word_view(word), STR_VIEW_LIT("wat2wasm"))) {
    cmd_word_t *in = word->next;
    cmd_word_t *out = in ? (in->next && str_view_eq(cmd_word_view(in->next), STR_VIEW_LIT("-o"))
                           ? in->next->next : in->next) : null;
    if (!in || !out) {
        screen_push_line("Usage: wat2wasm <input.wat> <output.wasm>");
        goto Label_Free;
    }

    // Zero-copy views from cmd_word_t — no kmalloc/kfree
    str_view_t in_sv  = cmd_word_view(in);
    str_view_t out_sv = cmd_word_view(out);

    screen_push_linef("wat2wasm: compiling %.*s -> %.*s",
                      in_sv.len,  in_sv.data,
                      out_sv.len, out_sv.data);

    // Convert to C strings only for the native argv interface
    char *argv[4] = {
        "wat2wasm",
        str_view_to_c(in_sv),
        "-o",
        str_view_to_c(out_sv),
    };
    wat2wasm_native(4, argv);
    kfree(argv[1]);
    kfree(argv[3]);
    goto Label_Free;
}
```

Once `wat2wasm_native` is updated to take `str_view_t *argv`, even the two remaining `str_view_to_c` calls go away.

---

## 8. Performance Impact

| Operation | Null-Terminated | View/Slice | Speedup |
|-----------|----------------|-------|---------|
| `strlen` | O(n) scan | O(1) field read | 10-100x |
| `strcmp` | 2× strlen + compare | 1× len check + memcmp (if lengths match) | 2-5x |
| Substring | malloc + memcpy + null | pointer arithmetic (free) | ∞ |
| `starts_with` | manual loop or strncmp | memcmp of min(len_a, prefix.len) | 2-3x |
| Path split | tokenize into stack buffer | `str_view_split` in-place | 5-10x |
| C interop (str_t) | free (.data is \0) | str_view_to_c (alloc+copy) | str_t wins |
| C interop (str_view_t) | free (already \0) | alloc+copy+null | C string wins |

For a kernel, the biggest win is `strlen`: every `screen_push_linef`, `serial_outsf`, and `cmd_word_eq` currently scans the input string. With views, the length is already known from parsing — no scanning needed.

---

## 9. Edge Cases and Safety

### 9.1 Bounds Checking

In debug builds (`#ifndef NDEBUG`), `str_view_at(sv, i)` asserts `i < sv.len`. In release, it's a raw pointer access (same as `sv.data[i]` — no overhead).

### 9.2 Dangling Views

A `str_view_t` points into memory owned elsewhere. If the owner is freed, the view dangles — same as any C pointer. This is a **lifetime bug**, not a type-system bug. The rule: never store a `str_view_t` that outlives its source.

Mitigations:
- `str_t` for heap-owned strings — always safe to store
- `str_view_t` for function parameters — the caller guarantees lifetime for the call duration
- Comments on functions that return `str_view_t`: document the lifetime (e.g., "valid until the next call to this function")

### 9.3 `str_view_t` → C Function (No Null Terminator)

A `str_view_t` is not safe to pass to `printf("%s", ...)` or any function expecting `\0`. Use `str_view_to_c()` to allocate a null-terminated copy, or better: use `str_t` if you own the data, since `str_t.data` is always null-terminated.

### 9.4 `str_t` Null-Terminator Invariant

`str_t` maintains `data[len] == '\0'` by:
- Allocating `cap >= len + 1`
- Writing `'\0'` after every mutation (`str_append`, `str_push`, `str_set`, `str_clear`)
- Never exposing a mutable pointer to `data[len]` — only read access through `str_view_from_str()`

---

## 10. Implementation Plan

| Phase | What | Effort | Lines |
|-------|------|--------|-------|
| **1** | `str_t`, `str_view_t`, core API (construction, query, comparison, slicing, conversion, mutation) | Medium | ~350 |
| **2** | Convert `cmd_word_t`, `util_cmd.c` to use `str_view_t` | Small | ~40 |
| **3** | Add `str_view_t` overloads for `fs_open`, `ext2_find_path`, etc. | Medium | ~100 |
| **4** | Convert terminal/screen/serial output to accept `str_view_t` | Medium | ~60 |
| **5** | Convert WASM host functions to use `str_view_t` for linear memory reads | Small | ~30 |
| **6** | Deprecate old `str_*` functions (keep as wrappers) | Trivial | ~20 |

**Total**: ~600 lines of new code, phased over multiple sessions. Each phase is self-contained and can be merged independently.

---

## 11. Open Questions

1. **Should `str_t.len` be `u32` or `u64`?** `u32` gives 4GB max string — plenty for a kernel. `u64` would be 24 bytes per `str_t` (worse cache). Start with `u32`, revisit if needed.

2. **Should we have a `str_fmt` that writes into a `str_t`?** A `str_printf(str_t *s, const char *fmt, ...)` wrapping `stbsp_vsnprintf` would be useful for building formatted strings without intermediate buffers. Phase 2+.

3. **`str_view_t` naming**: Could be `strref_t` or `str_slice_t` — settled on `str_view_t` because it's self-documenting and parallels `std::string_view`.

4. **Should file paths use `str_view_t` or stay `const char *` internally?** Changing `ext2_find_path` from `const char *` to `str_view_t` eliminates the stack-buffer tokenization. Worth doing in Phase 3.

---

## 12. Summary

Two types: `str_t` (owning, null-terminated) and `str_view_t` (borrowed slice). The key insight: **`str_t` is always null-terminated**, so `.data` drops into any existing C API. `str_view_t` is the zero-cost performance type for parsing, slicing, and passing strings around.

| | `str_t` | `str_view_t` |
|---|---|---|
| **Owns memory?** | Yes (kmalloc) | No |
| **Null-terminated?** | Always | Not guaranteed |
| **C interop** | Free (`.data`) | `str_view_to_c()` needed |
| **Substring cost** | Copy (via `str_from_view`) | Free (pointer arithmetic) |
| **Size** | 16 bytes | 12 bytes |
| **Use for** | Storage, building, C interop | Parameters, parsing, slicing |

The migration is incremental: add the types, then convert APIs one at a time. Old code keeps working via trivial `str_view_from_c()` wrappers.
