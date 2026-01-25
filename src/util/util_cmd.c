//
// Created by tobin on 2026-01-20.
//

#include "../include/util_cmd.h"
#include "../util/util_str.h"

// Changed allocator signature to size_t to match malloc, or you can cast malloc when calling
cmd_word_t *cmd_parse(const char *str, void *(*Alloc_Func)(u64)) {

    i32 strl = str_len(str);

    u8 inq = false;
    u8 esc = false;

    cmd_word_t *root = Alloc_Func(sizeof(cmd_word_t));
    root->loc = str; // Initialize to start of string (was 0)
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

        // If we are at the end of the string, stop here.
        if (c == '\0') {
            break;
        }

        // Otherwise, setup the NEXT word
        cmd_word_t *neww = Alloc_Func(sizeof(cmd_word_t));
        neww->loc = str + i + 1; // Start AFTER the space
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
