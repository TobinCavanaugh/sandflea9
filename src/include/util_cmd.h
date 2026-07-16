//
// Created by tobin on 2026-01-20.
//

#ifndef SANDFLEA9_UTIL_CMD_H
#define SANDFLEA9_UTIL_CMD_H

#include "dialect.h"
#include "../util/str_slice.h"

typedef enum {
    CMD_WT_STR,
    CMD_WT_i64,
    CMD_WT_ERR,
//    CMD_WT_F64
} CMD_WORD_TYPE;

typedef struct cmd_word_t {
    char *loc;
    u16 len;
    str_view_t text;   // view into loc/len — use word->text directly

    union {
        i64 val_i64;
        bool val_err;
        char *val_ignore;
    };

    CMD_WORD_TYPE val_type;

    struct cmd_word_t *next;
} cmd_word_t;

cmd_word_t *cmd_parse(const char *str, void *(*Alloc_Func)(u64));

// Borrow the word's text as a str_view_t — zero-copy, no allocation.
str_view_t cmd_word_view(const cmd_word_t *w);

// Compare a word against a C string literal. Uses str_view_eq internally
// for O(1) length check via word->len before the memcmp.
u8 cmd_word_eq(const cmd_word_t *word, const char *match);

u0 cmd_parse_free(cmd_word_t *root, void (*Free_Func)(void *));

#endif //SANDFLEA9_UTIL_CMD_H
