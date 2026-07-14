//
// Created by tobin on 2025-11-30.
//

#include "util_str.h"
#include "../include/kern_mem.h"

u32 str_len(const char *str) {
    i32 i = 0;
    while (str[i]) {
        i++;
    }
    return i;
}

u0 reverse(char s[]) {
    i32 i, j;
    char c;

    for (i = 0, j = str_len(s) - 1; i < j; i++, j--) {
        c = s[i];
        s[i] = s[j];
        s[j] = c;
    }
}

u32 i64_to_sn(i64 val, char *out_buf, u8 base, u32 max_size) {
    if (max_size == 0) {
        return 0;
    }

    char temp_buf[65]; // Sufficient for 64-bit binary + sign + null
    i32 i = 0;
    u64 u_val;
    int is_negative = 0;

    // 1. Handle Sign & Type Casting
    // We treat Base 10 as signed, and others (Hex, Binary) as unsigned/raw bits.
    // We cast to u64 to safely handle i64_MIN negation.
    if (base == 10 && val < 0) {
        is_negative = 1;
        u_val = (u64) (-(val + 1)) + 1; // 2's complement safe negation
    } else {
        u_val = (u64) val;
    }

    // 2. Handle Zero Explicitly
    if (u_val == 0) {
        temp_buf[i++] = '0';
    } else {
        // 3. Convert Digits
        while (u_val != 0) {
            int rem = u_val % base;
            if (rem < 10) {
                temp_buf[i++] = rem + '0';
            } else {
                temp_buf[i++] = (rem - 10) + 'A'; // Output 'A'-'F' for Hex
            }
            u_val /= base;
        }
    }

    // 4. Append Negative Sign
    if (is_negative) {
        temp_buf[i++] = '-';
    }

    // 5. Copy to Output Buffer (Reverse & Bounds Check)
    // We generated digits in reverse (LSD first), so we read temp_buf backward.
    u32 out_len = 0;
    while (i > 0 && out_len < max_size - 1) {
        out_buf[out_len++] = temp_buf[--i];
    }

    out_buf[out_len] = '\0'; // Null terminate

    return out_len;
}


u32 u64_to_sn(u64 val, char *out_buf, u8 base, u32 max_size) {
    if (max_size == 0) {
        return 0;
    }

    char temp_buf[65]; // Sufficient for 64-bit binary + null
    i32 i = 0;

    // 1. Handle Zero Explicitly
    if (val == 0) {
        temp_buf[i++] = '0';
    } else {
        // 2. Convert Digits
        while (val != 0) {
            int rem = val % base;
            if (rem < 10) {
                temp_buf[i++] = rem + '0';
            } else {
                temp_buf[i++] = (rem - 10) + 'A'; // Output 'A'-'F' for Hex
            }
            val /= base;
        }
    }

    // 3. Copy to Output Buffer (Reverse & Bounds Check)
    // We generated digits in reverse (LSD first), so we read temp_buf backward.
    u32 out_len = 0;
    while (i > 0 && out_len < max_size - 1) {
        out_buf[out_len++] = temp_buf[--i];
    }

    out_buf[out_len] = '\0'; // Null terminate

    return out_len;
}

purefn u8 str_eqla(const char *a, const char *b) {
    return str_eql(a, b, str_len(a));
}

purefn u8 str_eqlb(const char *a, const char *b) {
    return str_eql(a, b, str_len(b));
}

purefn u8 str_eql(const char *a, const char *b, u32 len) {
    if (a == b) return true;
    if (a == null || b == null) return false;

    for (u32 i = 0; i < len; i++) {
        if (a[i] != b[i]) return false;
        if (a[i] == '\0') return true;
        if (b[i] == '\0') return true;
    }

    return true;
}

purefn u8 str_sw(const char *a, const char *b) {
    if (a == null || b == null) return false;
    return str_eql(a, b, str_len(b));
}

purefn u8 str_eq(const char *a, const char *b) {
    i32 alen = str_len(a);
    i32 blen = str_len(b);

    if (alen != blen) { return false; }

    for (i32 i = 0; i < alen; i++) {
        if (a[i] != b[i]) { return false; }
    }

    return true;
}

char *str_dup(const char *a, void *(*Alloc_Func)(u64)) {
    i32 len = str_len(a);
    char *res = Alloc_Func(len + 1);
    mem_copy(res, a, len + 1);
    return res;
}

char *str_dup_len(const char *a, u32 len, void *(*Alloc_Func)(u64)) {
    char *res = Alloc_Func(len + 1);
    mem_copy(res, a, len);
    res[len] = 0;
    return res;
}

i64 sn_to_i64(const char *str, u32 max_len, u8 base) {
    if (max_len == 0 || base < 2 || base > 36) return 0;

    i64 result = 0;
    i64 sign = 1;
    u32 i = 0;

    // Handle sign
    if (str[0] == '-') {
        sign = -1;
        i = 1;
    } else if (str[0] == '+') {
        i = 1;
    }

    for (; i < max_len; i++) {
        char c = str[i];
        if (c == '\0') break;

        u8 digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'z') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'Z') {
            digit = c - 'A' + 10;
        } else {
            break; // Invalid character
        }

        if (digit >= base) break; // Digit not in range for base

        result = (result * base) + digit;
    }

    return result * sign;
}