//
// Created by tobin on 2026-01-20.
//

#include "../include/util_cmd.h"
#include "../util/util_str.h"

static bool is_numeric(const char *str, u32 len) {
    if (len == 0) return false;
    u32 i = 0;
    if (str[0] == '-' || str[0] == '+') {
        if (len == 1) return false;
        i = 1;
    }

    // Check for hex
    if (len > i + 2 && str[i] == '0' && (str[i+1] == 'x' || str[i+1] == 'X')) {
        for (u32 j = i + 2; j < len; j++) {
            char c = str[j];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
        }
        return true;
    }

    for (; i < len; i++) {
        if (str[i] < '0' || str[i] > '9') return false;
    }
    return true;
}

// Changed allocator signature to size_t to match malloc, or you can cast malloc when calling
cmd_word_t *cmd_parse(const char *str, void *(*Alloc_Func)(u64)) {

    i32 strl = str_len(str);

    u8 inq = false;
    u8 esc = false;

    cmd_word_t *root = Alloc_Func(sizeof(cmd_word_t));
    root->loc = (char*)str; // Initialize to start of string (was 0)
    root->val_type = CMD_WT_STR;
    root->next = null;

    cmd_word_t *current = root;

    // Loop until i <= strl to catch the null terminator
    for (int i = 0; i <= strl; i++) {
        char c = str[i];

        if (esc) {
            esc = false;
            continue;
        }
        if (c == '\\') {
            esc = true;
            continue;
        }

        if (c == '"') {
            inq = !inq;
            continue;
        }

        // Check for space OR null terminator (end of string)
        if ((!inq && c == ' ') || (c == '\0')) {
            goto New_Word;
        }

        continue;

        New_Word:
        // Calculate length of the CURRENT word
        current->len = (str + i) - current->loc;

        // Numeric Parsing logic
        if (is_numeric(current->loc, current->len)) {
            current->val_type = CMD_WT_i64;
            u32 offset = 0;
            u8 base = 10;
            const char* ptr = current->loc;
            u32 len = current->len;

            if (ptr[0] == '-' || ptr[0] == '+') {
                offset = 1;
            }

            if (len > offset + 2 && ptr[offset] == '0' && (ptr[offset+1] == 'x' || ptr[offset+1] == 'X')) {
                // Hex path
                // We pass the whole string to sn_to_i64, it handles sign. 
                // But we need to skip 0x for the numeric part if sn_to_i64 doesn't support 0x.
                // Our sn_to_i64 breaks on 'x'. So we parse the sign then skip 0x.
                i64 sign = (ptr[0] == '-') ? -1 : 1;
                current->val_i64 = sign * sn_to_i64(ptr + offset + 2, len - offset - 2, 16);
            } else {
                current->val_i64 = sn_to_i64(ptr, len, 10);
            }
        }

        // If we are at the end of the string, stop here.
        if (c == '\0') {
            break;
        }

        // Otherwise, setup the NEXT word
        cmd_word_t *neww = Alloc_Func(sizeof(cmd_word_t));
        neww->loc = (char*)(str + i + 1); // Start AFTER the space
        neww->val_type = CMD_WT_STR; // Initialize defaults for new node
        neww->next = null;

        current->next = neww;
        current = neww;
    }

    return root; // RETURN the list!
}

u0 cmd_parse_free(cmd_word_t *root, void (*Free_Func)(void *)) {
    cmd_word_t *next = root;
    while (next != null) {
        cmd_word_t *tmp = next->next;
        Free_Func(next);
        next = tmp;
    }
}
