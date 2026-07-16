//
// str_slice.h — String types for sandfleaOS
//
// Two types:
//   str_view_t  — non-owning slice {data, len}. 12 bytes. Zero-copy substrings.
//   str_t       — owning string {data, len, cap}. 16 bytes. Always null-terminated.
//
// str_t.data is ALWAYS null-terminated — pass it directly to C functions.
// str_view_t is NOT null-terminated — use str_view_to_c() or convert to str_t.

#ifndef SANDFLEA_STR_SLICE_H
#define SANDFLEA_STR_SLICE_H

#include "../include/dialect.h"

// ============================================================================
// Types
// ============================================================================

// Non-owning view into string data. Points into someone else's memory.
// NOT null-terminated (may be a substring in the middle of a buffer).
typedef struct {
    const char *data;
    u32         len;
} str_view_t;

// Owning, heap-allocated, always null-terminated string.
// Invariant: data[len] == '\0' and cap >= len + 1.
typedef struct {
    char *data;
    u32   len;
    u32   cap;
} str_t;

// Split result from str_view_split_once
typedef struct {
    str_view_t left;
    str_view_t right;
} str_view_split_t;

// ============================================================================
// Compile-time literal macro
// ============================================================================

// Produces a str_view_t from a C string literal at compile time. Zero cost.
// Example:  str_view_t name = STR_VIEW_LIT("hello");
//           → { .data = "hello", .len = 5 }
#define STR_VIEW_LIT(c_string_literal) \
    ((str_view_t){ .data = (c_string_literal), .len = sizeof(c_string_literal) - 1 })

// ============================================================================
// str_view_t — Construction
// ============================================================================

// From a null-terminated C string (scans for length — use sparingly).
str_view_t str_view_from_c(const char *s);

// From pointer + length pair (no scan — preferred).
str_view_t str_view_from_parts(const char *data, u32 len);

// Borrow from a str_t. Returned view is valid as long as `s` lives.
str_view_t str_view_from_str(const str_t *s);

// ============================================================================
// str_view_t — Query (all O(1))
// ============================================================================

u32  str_view_len(str_view_t sv);
bool str_view_is_empty(str_view_t sv);
char str_view_at(str_view_t sv, u32 i);

// ============================================================================
// str_view_t — Comparison
// ============================================================================

bool str_view_eq(str_view_t a, str_view_t b);
bool str_view_starts_with(str_view_t sv, str_view_t prefix);
bool str_view_ends_with(str_view_t sv, str_view_t suffix);
i32  str_view_cmp(str_view_t a, str_view_t b);

// ============================================================================
// str_view_t — Slicing (zero-copy, all O(1))
// ============================================================================

// Substring from offset to end. Clamps start to sv.len.
str_view_t str_view_slice_from(str_view_t sv, u32 start);

// Substring of `len` bytes starting at `start`. Clamps to bounds.
str_view_t str_view_slice(str_view_t sv, u32 start, u32 len);

// Split on the first occurrence of delimiter.
// If not found, returns {sv, {NULL, 0}}.
str_view_split_t str_view_split_once(str_view_t sv, char delimiter);

// Split on delimiter into up to `max_out` components.
// Returns the number of components written (may be less than max_out).
// Empty components are included (e.g., "/a//b/" → {"", "a", "", "b", ""}).
u32 str_view_split(str_view_t sv, char delimiter, str_view_t *out, u32 max_out);

// Trim leading and trailing whitespace (' ', '\t', '\n', '\r').
str_view_t str_view_trim(str_view_t sv);

// ============================================================================
// str_view_t — Search
// ============================================================================

// Find first occurrence of `c`. Returns offset, or -1 if not found.
i32 str_view_find(str_view_t sv, char c);

// Find last occurrence of `c`. Returns offset, or -1 if not found.
i32 str_view_find_last(str_view_t sv, char c);

// ============================================================================
// str_view_t — Conversion to C string
// ============================================================================

// Allocates a null-terminated copy on the heap. Caller must kfree().
// Use only when you MUST pass to a function expecting null-terminated strings.
char *str_view_to_c(str_view_t sv);

// Writes into a pre-allocated buffer. Always null-terminates.
// Returns number of bytes written (excluding '\0'), or 0 if out_cap is too small.
u32 str_view_to_buf(str_view_t sv, char *out, u32 out_cap);

// ============================================================================
// str_t — Construction and destruction
// ============================================================================

// Create empty string with initial capacity.
// `cap` includes the null terminator byte, so minimum useful cap is 1.
str_t str_new(u32 cap);

// Create by copying a str_view_t. Always null-terminates.
str_t str_from_view(str_view_t sv);

// Create by copying a null-terminated C string.
str_t str_from_c(const char *s);

// Free the heap allocation and zero the struct.
void str_free(str_t *s);

// ============================================================================
// str_t — Query (all O(1))
// ============================================================================

u32  str_len_s(const str_t *s);
u32  str_cap(const str_t *s);
bool str_is_empty(const str_t *s);
char str_at(const str_t *s, u32 i);

// Borrow as a str_view_t. The returned view is valid as long as `s` lives.
// This is the primary way to pass a str_t to functions accepting str_view_t.
#define str_view(s) str_view_from_str(s)

// ============================================================================
// str_t — Mutation
// ============================================================================

// Append a str_view_t. Grows capacity if needed. Maintains null-termination.
void str_append(str_t *s, str_view_t sv);

// Append a single character. Grows capacity if needed.
void str_push(str_t *s, char c);

// Set contents from a str_view_t (replaces existing data).
void str_set(str_t *s, str_view_t sv);

// Clear contents but keep the allocation. Sets len=0, data[0]='\0'.
void str_clear(str_t *s);

// Ensure capacity for at least `new_cap` bytes (including '\0').
// Does nothing if already large enough.
void str_reserve(str_t *s, u32 new_cap);

// Shrink capacity to exactly len + 1.
void str_shrink(str_t *s);

// ============================================================================
// str_t — Comparison (convenience wrappers)
// ============================================================================

// Compare a str_t with a str_view_t.
bool str_eq_view(const str_t *s, str_view_t sv);

// Compare two str_t instances.
bool str_eq_str(const str_t *a, const str_t *b);

#endif // SANDFLEA_STR_SLICE_H
