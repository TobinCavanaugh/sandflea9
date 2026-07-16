//
// str_slice.c — String types for sandfleaOS
//
// Implementation of str_view_t (non-owning slice) and str_t (owning,
// null-terminated string). Uses kmalloc/kern_realloc/kfree from kern_vmm.h
// and mem_copy/mem_set from kern_mem.h.

#include "str_slice.h"
#include "../include/kern_vmm.h"
#include "../include/kern_mem.h"

// ============================================================================
// Helpers
// ============================================================================

// Minimum capacity for a non-empty string: len + 1 (for '\0').
// New empty strings start with cap 16 for small-string optimization.
#define STR_MIN_CAP 16

// Growth factor: double capacity, but at least 16 bytes.
static u32 str_grow_cap(u32 current, u32 needed) {
    u32 next = current ? current * 2 : STR_MIN_CAP;
    return next > needed ? next : needed;
}

// Simple memcmp replacement (we have memcmp in string.h → stbsupport.c,
// but str_slice.c should stand alone without that include chain).
static i32 local_memcmp(const u8 *a, const u8 *b, u32 n) {
    for (u32 i = 0; i < n; i++) {
        if (a[i] != b[i]) return (i32)a[i] - (i32)b[i];
    }
    return 0;
}

static bool is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// ============================================================================
// str_view_t — Construction
// ============================================================================

str_view_t str_view_from_c(const char *s) {
    str_view_t sv;
    sv.data = s;
    sv.len  = 0;
    if (s) {
        while (s[sv.len]) sv.len++;
    }
    return sv;
}

str_view_t str_view_from_parts(const char *data, u32 len) {
    str_view_t sv;
    sv.data = data;
    sv.len  = len;
    return sv;
}

str_view_t str_view_from_str(const str_t *s) {
    str_view_t sv;
    sv.data = s ? s->data : null;
    sv.len  = s ? s->len  : 0;
    return sv;
}

// ============================================================================
// str_view_t — Query
// ============================================================================

u32 str_view_len(str_view_t sv) {
    return sv.len;
}

bool str_view_is_empty(str_view_t sv) {
    return sv.len == 0;
}

char str_view_at(str_view_t sv, u32 i) {
    if (i >= sv.len) return 0;
    return sv.data[i];
}

// ============================================================================
// str_view_t — Comparison
// ============================================================================

bool str_view_eq(str_view_t a, str_view_t b) {
    if (a.data == b.data && a.len == b.len) return true;
    if (a.len != b.len) return false;
    if (a.data == null || b.data == null) return false;
    return local_memcmp((const u8 *)a.data, (const u8 *)b.data, a.len) == 0;
}

bool str_view_starts_with(str_view_t sv, str_view_t prefix) {
    if (prefix.len > sv.len) return false;
    if (prefix.len == 0) return true;
    return local_memcmp((const u8 *)sv.data, (const u8 *)prefix.data, prefix.len) == 0;
}

bool str_view_ends_with(str_view_t sv, str_view_t suffix) {
    if (suffix.len > sv.len) return false;
    if (suffix.len == 0) return true;
    return local_memcmp((const u8 *)(sv.data + sv.len - suffix.len),
                        (const u8 *)suffix.data, suffix.len) == 0;
}

i32 str_view_cmp(str_view_t a, str_view_t b) {
    if (a.data == b.data && a.len == b.len) return 0;
    u32 min_len = a.len < b.len ? a.len : b.len;
    i32 cmp = local_memcmp((const u8 *)a.data, (const u8 *)b.data, min_len);
    if (cmp != 0) return cmp;
    if (a.len < b.len) return -1;
    if (a.len > b.len) return 1;
    return 0;
}

// ============================================================================
// str_view_t — Slicing
// ============================================================================

str_view_t str_view_slice_from(str_view_t sv, u32 start) {
    if (start >= sv.len) {
        return str_view_from_parts(sv.data + sv.len, 0);
    }
    return str_view_from_parts(sv.data + start, sv.len - start);
}

str_view_t str_view_slice(str_view_t sv, u32 start, u32 len) {
    if (start >= sv.len) {
        return str_view_from_parts(sv.data + sv.len, 0);
    }
    u32 max_len = sv.len - start;
    if (len > max_len) len = max_len;
    return str_view_from_parts(sv.data + start, len);
}

str_view_split_t str_view_split_once(str_view_t sv, char delimiter) {
    str_view_split_t result;
    i32 idx = str_view_find(sv, delimiter);
    if (idx < 0) {
        result.left  = sv;
        result.right = str_view_from_parts(sv.data + sv.len, 0);
    } else {
        result.left  = str_view_slice(sv, 0, (u32)idx);
        result.right = str_view_slice_from(sv, (u32)(idx + 1));
    }
    return result;
}

u32 str_view_split(str_view_t sv, char delimiter, str_view_t *out, u32 max_out) {
    if (max_out == 0) return 0;

    u32 count  = 0;
    u32 start  = 0;

    for (u32 i = 0; i < sv.len && count < max_out; i++) {
        if (sv.data[i] == delimiter) {
            out[count++] = str_view_slice(sv, start, i - start);
            start = i + 1;
        }
    }

    // Final component
    if (count < max_out) {
        out[count++] = str_view_slice_from(sv, start);
    }

    return count;
}

str_view_t str_view_trim(str_view_t sv) {
    u32 start = 0;
    u32 end   = sv.len;

    while (start < end && is_whitespace(sv.data[start])) start++;
    while (end > start && is_whitespace(sv.data[end - 1])) end--;

    return str_view_slice(sv, start, end - start);
}

// ============================================================================
// str_view_t — Search
// ============================================================================

i32 str_view_find(str_view_t sv, char c) {
    for (u32 i = 0; i < sv.len; i++) {
        if (sv.data[i] == c) return (i32)i;
    }
    return -1;
}

i32 str_view_find_last(str_view_t sv, char c) {
    for (u32 i = sv.len; i > 0; i--) {
        if (sv.data[i - 1] == c) return (i32)(i - 1);
    }
    return -1;
}

// ============================================================================
// str_view_t — Conversion
// ============================================================================

char *str_view_to_c(str_view_t sv) {
    if (!sv.data) return null;
    char *buf = (char *)kmalloc(sv.len + 1);
    if (!buf) return null;
    mem_copy((u8 *)buf, (const u8 *)sv.data, sv.len);
    buf[sv.len] = '\0';
    return buf;
}

u32 str_view_to_buf(str_view_t sv, char *out, u32 out_cap) {
    if (out_cap == 0) return 0;
    if (sv.len >= out_cap) {
        // Not enough room for data + null
        return 0;
    }
    mem_copy((u8 *)out, (const u8 *)sv.data, sv.len);
    out[sv.len] = '\0';
    return sv.len;
}

// ============================================================================
// str_t — Internal helpers
// ============================================================================

// Reallocate a str_t's buffer to at least new_cap bytes.
// Preserves contents. Updates s->data, s->cap. s->len unchanged.
static void str_realloc(str_t *s, u32 new_cap) {
    if (new_cap <= s->cap) return;

    // Ensure minimum reasonable capacity
    if (new_cap < STR_MIN_CAP) new_cap = STR_MIN_CAP;

    char *new_data = (char *)kern_realloc(s->data, new_cap);
    if (!new_data) return;  // OOM — leave s unchanged

    s->data = new_data;
    s->cap  = new_cap;
}

// Ensure s has room for `additional` more bytes PLUS the null terminator.
static void str_ensure_cap(str_t *s, u32 additional) {
    u32 needed = s->len + additional + 1;  // +1 for '\0'
    if (needed > s->cap) {
        str_realloc(s, str_grow_cap(s->cap, needed));
    }
}

// ============================================================================
// str_t — Construction and destruction
// ============================================================================

str_t str_new(u32 cap) {
    str_t s;
    if (cap < STR_MIN_CAP) cap = STR_MIN_CAP;
    s.data = (char *)kmalloc(cap);
    s.len  = 0;
    s.cap  = s.data ? cap : 0;
    if (s.data) s.data[0] = '\0';
    return s;
}

str_t str_from_view(str_view_t sv) {
    str_t s;
    // Treat null data as empty — avoids uninitialized bytes in the output.
    if (!sv.data) sv.len = 0;
    u32 cap = sv.len + 1;
    if (cap < STR_MIN_CAP) cap = STR_MIN_CAP;
    s.data = (char *)kmalloc(cap);
    s.len  = sv.len;
    s.cap  = s.data ? cap : 0;
    if (s.data) {
        if (sv.data && sv.len > 0) {
            mem_copy((u8 *)s.data, (const u8 *)sv.data, sv.len);
        }
        s.data[sv.len] = '\0';
    }
    return s;
}

str_t str_from_c(const char *s) {
    str_view_t sv = str_view_from_c(s);
    return str_from_view(sv);
}

void str_free(str_t *s) {
    if (s && s->data) {
        kfree(s->data);
        s->data = null;
        s->len  = 0;
        s->cap  = 0;
    }
}

// ============================================================================
// str_t — Query
// ============================================================================

u32 str_len_s(const str_t *s) {
    return s ? s->len : 0;
}

u32 str_cap(const str_t *s) {
    return s ? s->cap : 0;
}

bool str_is_empty(const str_t *s) {
    return !s || s->len == 0;
}

char str_at(const str_t *s, u32 i) {
    if (!s || i >= s->len) return 0;
    return s->data[i];
}

// ============================================================================
// str_t — Mutation
// ============================================================================

void str_append(str_t *s, str_view_t sv) {
    if (!s || !s->data || sv.len == 0 || !sv.data) return;

    str_ensure_cap(s, sv.len);

    if (sv.data && sv.len > 0) {
        mem_copy((u8 *)(s->data + s->len), (const u8 *)sv.data, sv.len);
    }
    s->len += sv.len;
    s->data[s->len] = '\0';
}

void str_push(str_t *s, char c) {
    if (!s || !s->data) return;

    str_ensure_cap(s, 1);

    s->data[s->len++] = c;
    s->data[s->len]   = '\0';
}

void str_set(str_t *s, str_view_t sv) {
    if (!s || !s->data) return;

    // Check if we need to grow
    u32 needed = sv.len + 1;
    if (needed > s->cap) {
        str_realloc(s, needed > STR_MIN_CAP ? needed : STR_MIN_CAP);
        if (!s->data) return;  // OOM during realloc
    }

    if (sv.data && sv.len > 0) {
        // Handle overlap: if sv points into s->data, use mem_move
        if (sv.data >= s->data && sv.data < s->data + s->cap) {
            mem_move((u8 *)s->data, (const u8 *)sv.data, sv.len);
        } else {
            mem_copy((u8 *)s->data, (const u8 *)sv.data, sv.len);
        }
    }
    s->len = sv.len;
    s->data[s->len] = '\0';
}

void str_clear(str_t *s) {
    if (!s || !s->data) return;
    s->len = 0;
    s->data[0] = '\0';
}

void str_reserve(str_t *s, u32 new_cap) {
    if (!s) return;
    if (new_cap > s->cap) {
        str_realloc(s, new_cap);
    }
}

void str_shrink(str_t *s) {
    if (!s || !s->data) return;
    u32 exact = s->len + 1;
    if (exact < s->cap) {
        char *new_data = (char *)kern_realloc(s->data, exact);
        if (new_data) {
            s->data = new_data;
            s->cap  = exact;
        }
        // If kern_realloc fails, keep the larger buffer — harmless
    }
}

// ============================================================================
// str_t — Comparison
// ============================================================================

bool str_eq_view(const str_t *s, str_view_t sv) {
    if (!s) return sv.len == 0;
    return str_view_eq(str_view_from_str(s), sv);
}

bool str_eq_str(const str_t *a, const str_t *b) {
    return str_view_eq(str_view_from_str(a), str_view_from_str(b));
}
