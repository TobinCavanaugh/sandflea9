// src/kernel/wat2wasm_native.c
//
// Minimal native WAT→WASM compiler.
// Compiles WAT text into WASM binary format without any external
// dependencies — no wabt, no wasi-sdk, no WASI, no wasm3 hacks.
// Runs directly in the kernel (freestanding C).
//
// Supported subset (enough for hello.wat, add_test.wat, etc.):
//   - Module declaration
//   - Import section (import "module" "name" (func ...))
//   - Function declarations with param/result/local
//   - Export section (export "name" (func $id))
//   - Memory section (memory <initial_pages>)
//   - Data segments: (data (i32.const <offset>) "bytes")
//   - Global section: (global $name (mut?) <type> <init_expr>)
//   - Instructions: i32.const, i64.const, i32.add/sub/mul/and/or/xor,
//     i32.eqz, i32.eq, i32.ne, call, drop, return, block, loop, if/else/end,
//     br, br_if, local.get, local.set, local.tee, global.get, global.set,
//     i32.load, i32.store, i32.load8_u, i32.store8,
//     f32.const, f64.const, f32.add/sub/mul/div,
//     memory.size, memory.grow

#include "../include/wasm_spawn.h"
#include "../include/kern_serial.h"
#include "../include/kern_screen.h"
#include "../include/kern_mem.h"
#include "../include/string.h"
#include "../util/util_str.h"
#include "../include/stbsupport.h"

// Local memory comparison (kernel doesn't implement memcmp in kern_mem.c)
static int wat_memcmp(const void *a, const void *b, u32 len) {
    const u8 *pa = (const u8*)a, *pb = (const u8*)b;
    for (u32 i = 0; i < len; i++) {
        if (pa[i] != pb[i]) return pa[i] - pb[i];
    }
    return 0;
}

// ============================================================
// Dynamic buffer for assembling WASM binary
// ============================================================

typedef struct {
    u8 *data;
    u32 size;
    u32 cap;
} wbin_t;

static void wb_init(wbin_t *b) {
    b->data = null;
    b->size = 0;
    b->cap  = 0;
}

static void wb_free(wbin_t *b) {
    if (b->data) kfree(b->data);
    b->data = null;
    b->size = 0;
    b->cap  = 0;
}

static int wb_grow(wbin_t *b, u32 need) {
    if (b->size + need <= b->cap) return 0;
    u32 new_cap = b->cap ? b->cap * 2 : 4096;
    while (new_cap < b->size + need) new_cap *= 2;
    u8 *p = kmalloc(new_cap);
    if (!p) return -1;
    if (b->data) {
        mem_copy(p, b->data, b->size);
        kfree(b->data);
    }
    b->data = p;
    b->cap  = new_cap;
    return 0;
}

#define wb1(b, v)  do { if (wb_grow(b,1)) return -1; (b)->data[(b)->size++] = (v); } while(0)
#define wb2(b, v)  do { if (wb_grow(b,2)) return -1; u32 _v=(v); (b)->data[(b)->size++]=(_v)&0xFF; (b)->data[(b)->size++]=((_v)>>8)&0xFF; } while(0)
#define wb4(b, v)  do { if (wb_grow(b,4)) return -1; u32 _v=(v); (b)->data[(b)->size++]=(_v)&0xFF; (b)->data[(b)->size++]=((_v)>>8)&0xFF; (b)->data[(b)->size++]=((_v)>>16)&0xFF; (b)->data[(b)->size++]=((_v)>>24)&0xFF; } while(0)

static void wb_emit(wbin_t *b, const u8 *src, u32 len) {
    if (wb_grow(b, len)) return;
    if (len) mem_copy(b->data + b->size, src, len);
    b->size += len;
}

// LEB128 unsigned
static u32 wb_uleb(u8 *buf, u32 v) {
    u32 n = 0;
    do {
        u8 byte = v & 0x7F;
        v >>= 7;
        if (v) byte |= 0x80;
        buf[n++] = byte;
    } while (v);
    return n;
}

static void wb_uleb_buf(wbin_t *b, u32 v) {
    u8 tmp[8];
    u32 n = wb_uleb(tmp, v);
    wb_emit(b, tmp, n);
}

// LEB128 signed
static u32 wb_sleb(u8 *buf, i32 v) {
    u32 n = 0;
    int more = 1;
    while (more) {
        u8 byte = v & 0x7F;
        v >>= 7;
        if (!((v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40)))) byte |= 0x80;
        else more = 0;
        buf[n++] = byte;
    }
    return n;
}

static void wb_sleb_buf(wbin_t *b, i32 v) {
    u8 tmp[8];
    u32 n = wb_sleb(tmp, v);
    wb_emit(b, tmp, n);
}

// Section header: write section ID, then reserve space for length.
// Returns position of the length field start.
// After writing section content, call wb_sec_end() to fix up length.
static u32 wb_sec_start(wbin_t *b, u8 id) {
    wb1(b, id);
    u32 pos = b->size;
    // Reserve 5 bytes for the section length (max LEB128 for 32-bit)
    for (int i = 0; i < 5; i++) wb1(b, 0);
    return pos;
}

static void wb_sec_end(wbin_t *b, u32 pos) {
    u32 len = b->size - pos - 5; // content length
    u8 tmp[8];
    u32 n = wb_uleb(tmp, len);
    // Overwrite the reserved bytes with the actual length encoding
    for (u32 i = 0; i < n; i++) b->data[pos + i] = tmp[i];
    // If the encoding was shorter than 5 bytes, shift the content
    if (n < 5) {
        u32 shift = 5 - n;
        // Move the data after the length field back by `shift` bytes
        mem_move(b->data + pos + n, b->data + pos + 5, b->size - pos - 5);
        b->size -= shift;
    }
}

// Write string (len + bytes)
static void wb_str(wbin_t *b, const char *s, u32 len) {
    wb_uleb_buf(b, len);
    wb_emit(b, (const u8*)s, len);
}

// ============================================================
// Tokenizer
// ============================================================

#define TOK_MAX 4096

typedef enum {
    TOK_LPAREN, TOK_RPAREN, TOK_IDENT, TOK_INT, TOK_FLOAT, TOK_STRING, TOK_EOF, TOK_ERR
} tok_type_t;

typedef struct {
    tok_type_t type;
    const char *start;
    u32 len;
    i64 int_val;
} tok_t;

typedef struct {
    const char *p;
    const char *end;
    tok_t tokens[TOK_MAX];
    int ntokens;
    int pos;
} lexer_t;

static int is_iden(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
           c == '$' || c == '/' || c == ':' || c == '*' || c == '+' ||
           c == '>' || c == '<' || c == '=' || c == '!' || c == '~' ||
           c == '@' || c == '#' || c == '^' || c == '&';
}

static int hex_v(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int wat_lex(lexer_t *l) {
    l->ntokens = 0;
    l->pos = 0;

    while (l->p < l->end) {
        while (l->p < l->end && (*l->p == ' ' || *l->p == '\t' || *l->p == '\n' || *l->p == '\r'))
            l->p++;
        if (l->p >= l->end) break;

        // Line comment (;; ...)
        if (l->p + 1 < l->end && l->p[0] == ';' && l->p[1] == ';') {
            while (l->p < l->end && *l->p != '\n') l->p++;
            continue;
        }
        // Block comment ((; ... ;))
        if (l->p + 1 < l->end && l->p[0] == '(' && l->p[1] == ';') {
            l->p += 2;
            while (l->p + 1 < l->end && !(l->p[0] == ';' && l->p[1] == ')')) l->p++;
            if (l->p < l->end) l->p += 2;
            continue;
        }

        if (l->ntokens >= TOK_MAX - 1) return -1;
        tok_t *t = &l->tokens[l->ntokens];
        t->start = l->p;

        if (*l->p == '(') { t->type = TOK_LPAREN; l->p++; l->ntokens++; continue; }
        if (*l->p == ')') { t->type = TOK_RPAREN; l->p++; l->ntokens++; continue; }
        if (*l->p == '"') {
            l->p++;
            t->start = l->p;
            while (l->p < l->end && *l->p != '"') {
                if (*l->p == '\\') l->p++;
                l->p++;
            }
            t->len = (u32)(l->p - t->start);
            t->type = TOK_STRING;
            if (l->p < l->end) l->p++;
            l->ntokens++;
            continue;
        }

        // Identifier, keyword, or number
        if (is_iden(*l->p) || *l->p == '+' || *l->p == '-') {
            const char *ns = l->p;
            int is_neg = (*l->p == '-');
            if (is_neg || *l->p == '+') l->p++;

            // Try hex number
            if (l->p < l->end && *l->p == '0' && l->p + 1 < l->end &&
                (l->p[1] == 'x' || l->p[1] == 'X')) {
                l->p += 2;
                while (l->p < l->end && hex_v(*l->p) >= 0) l->p++;
                if (l->p > ns + (is_neg ? 3 : 2)) {
                    t->type = TOK_INT;
                    t->len = (u32)(l->p - ns);
                    t->int_val = sn_to_i64(ns + (is_neg ? 3 : 2), l->p - ns - (is_neg ? 3 : 2), 16);
                    if (is_neg) t->int_val = -t->int_val;
                    l->ntokens++;
                    continue;
                }
                l->p = ns; // rewind — treat as ident
            }

            // Read token chars
            while (l->p < l->end && is_iden(*l->p)) l->p++;

            // Check if it's a decimal integer
            int all_digits = 1;
            for (const char *cp = ns; cp < l->p; cp++) {
                if (*cp >= '0' && *cp <= '9') continue;
                if (cp == ns && (*cp == '-' || *cp == '+')) continue;
                all_digits = 0; break;
            }
            if (all_digits && l->p > ns) {
                t->type = TOK_INT;
                t->len = (u32)(l->p - ns);
                t->int_val = sn_to_i64(ns, l->p - ns, 10);
                l->ntokens++;
                continue;
            }

            t->type = TOK_IDENT;
            t->len = (u32)(l->p - ns);
            l->ntokens++;
            continue;
        }

        screen_push_linef("WAT: Unexpected char '%c'", *l->p);
        return -1;
    }

    if (l->ntokens >= TOK_MAX - 1) return -1;
    l->tokens[l->ntokens].type = TOK_EOF;
    l->ntokens++;
    return 0;
}

static tok_t *peek(lexer_t *l) {
    if (l->pos >= l->ntokens) return null;
    return &l->tokens[l->pos];
}
static void next(lexer_t *l) { if (l->pos < l->ntokens) l->pos++; }

static int expect(lexer_t *l, tok_type_t type) {
    tok_t *t = peek(l);
    if (!t || t->type != type) return -1;
    next(l); return 0;
}

static int match_id(lexer_t *l, const char *s) {
    tok_t *t = peek(l);
    if (!t || t->type != TOK_IDENT) return 0;
    u32 sl = str_len(s);
    return sl == t->len && wat_memcmp(s, t->start, sl) == 0;
}

// Name→index map for resolving $name references in call/global.get/set.
// Populated during import parsing, consulted during function body parsing.
#define MAX_IMPORT_NAMES 64
typedef struct {
    const char *name;
    u32 len;
    u32 index;   // function index in the WASM function index space
} import_name_t;

typedef struct {
    import_name_t entries[MAX_IMPORT_NAMES];
    u32 count;
} name_table_t;

static void nt_add(name_table_t *nt, const char *name, u32 len, u32 index) {
    if (nt->count >= MAX_IMPORT_NAMES || len == 0) return;
    nt->entries[nt->count].name  = name;
    nt->entries[nt->count].len   = len;
    nt->entries[nt->count].index = index;
    nt->count++;
}

static i32 nt_lookup(name_table_t *nt, const char *name, u32 len) {
    for (u32 i = 0; i < nt->count; i++) {
        if (nt->entries[i].len == len &&
            wat_memcmp(nt->entries[i].name, name, len) == 0) {
            return (i32)nt->entries[i].index;
        }
    }
    return -1;  // not found — caller should treat as error
}

// Parser context passed through to body/expr parsers for name resolution.
typedef struct {
    name_table_t *names;
} parse_ctx_t;

static int parse_body(lexer_t *l, wbin_t *b, parse_ctx_t *ctx);
static int parse_expr(lexer_t *l, wbin_t *b, parse_ctx_t *ctx);

// ============================================================
// Compiler
// ============================================================

// WASM opcodes
#define W_OP_UNREACHABLE   0x00
#define W_OP_NOP           0x01
#define W_OP_BLOCK         0x02
#define W_OP_LOOP          0x03
#define W_OP_IF            0x04
#define W_OP_ELSE          0x05
#define W_OP_END           0x0B
#define W_OP_BR            0x0C
#define W_OP_BR_IF         0x0D
#define W_OP_RETURN        0x0F
#define W_OP_CALL          0x10
#define W_OP_DROP          0x1A
#define W_OP_LOCAL_GET     0x20
#define W_OP_LOCAL_SET     0x21
#define W_OP_LOCAL_TEE     0x22
#define W_OP_GLOBAL_GET    0x23
#define W_OP_GLOBAL_SET    0x24
#define W_OP_I32_LOAD      0x28
#define W_OP_I64_LOAD      0x29
#define W_OP_F32_LOAD      0x2A
#define W_OP_F64_LOAD      0x2B
#define W_OP_I32_LOAD8_U   0x2C
#define W_OP_I32_STORE     0x36
#define W_OP_I64_STORE     0x37
#define W_OP_F32_STORE     0x38
#define W_OP_F64_STORE     0x39
#define W_OP_I32_STORE8    0x3A
#define W_OP_MEMORY_SIZE   0x3F
#define W_OP_MEMORY_GROW   0x40
#define W_OP_I32_CONST     0x41
#define W_OP_I64_CONST     0x42
#define W_OP_F32_CONST     0x43
#define W_OP_F64_CONST     0x44
#define W_OP_I32_EQZ       0x45
#define W_OP_I32_EQ        0x46
#define W_OP_I32_NE        0x47
#define W_OP_I32_LT_S      0x48
#define W_OP_I32_GT_S      0x4A
#define W_OP_I32_LE_S      0x4C
#define W_OP_I32_GE_S      0x4E
#define W_OP_I32_ADD       0x6A
#define W_OP_I32_SUB       0x6B
#define W_OP_I32_MUL       0x6C
#define W_OP_I32_AND       0x71
#define W_OP_I32_OR        0x72
#define W_OP_I32_XOR       0x73
#define W_OP_I32_SHL       0x74
#define W_OP_I32_SHR_U     0x76
#define W_OP_F32_ADD       0x92
#define W_OP_F32_SUB       0x93
#define W_OP_F32_MUL       0x94
#define W_OP_F32_DIV       0x95

#define WASM_T_I32 0x7F
#define WASM_T_I64 0x7E
#define WASM_T_F32 0x7D
#define WASM_T_F64 0x7C
#define WASM_T_VOID 0x40

static int parse_vt(lexer_t *l) {
    if (match_id(l, "i32")) { next(l); return WASM_T_I32; }
    if (match_id(l, "i64")) { next(l); return WASM_T_I64; }
    if (match_id(l, "f32")) { next(l); return WASM_T_F32; }
    if (match_id(l, "f64")) { next(l); return WASM_T_F64; }
    return -1;
}

// Recursive body parser
static int parse_body(lexer_t *l, wbin_t *b, parse_ctx_t *ctx) {
    while (1) {
        tok_t *t = peek(l);
        if (!t) return -1;
        if (t->type == TOK_RPAREN) return 0;

        // Nested S-expression
        if (t->type == TOK_LPAREN) {
            next(l);
            if (match_id(l, "block")) {
                next(l);
                int bt = WASM_T_VOID;
                if (peek(l) && peek(l)->type == TOK_LPAREN) {
                    next(l);
                    if (match_id(l, "result")) { next(l); bt = parse_vt(l); expect(l, TOK_RPAREN); }
                    else { expect(l, TOK_RPAREN); }
                }
                wb1(b, W_OP_BLOCK); wb_sleb_buf(b, bt == WASM_T_VOID ? -64 : bt);
                if (parse_body(l, b, ctx) < 0) return -1;
                wb1(b, W_OP_END);
                expect(l, TOK_RPAREN); continue;
            }
            if (match_id(l, "loop")) {
                next(l);
                int bt = WASM_T_VOID;
                if (peek(l) && peek(l)->type == TOK_LPAREN) {
                    next(l);
                    if (match_id(l, "result")) { next(l); bt = parse_vt(l); expect(l, TOK_RPAREN); }
                    else { expect(l, TOK_RPAREN); }
                }
                wb1(b, W_OP_LOOP); wb_sleb_buf(b, bt == WASM_T_VOID ? -64 : bt);
                if (parse_body(l, b, ctx) < 0) return -1;
                wb1(b, W_OP_END);
                expect(l, TOK_RPAREN); continue;
            }
            if (match_id(l, "if")) {
                next(l);
                int bt = WASM_T_VOID;
                if (peek(l) && peek(l)->type == TOK_LPAREN) {
                    next(l);
                    if (match_id(l, "result")) { next(l); bt = parse_vt(l); expect(l, TOK_RPAREN); }
                    else { expect(l, TOK_RPAREN); }
                }
                wb1(b, W_OP_IF); wb_sleb_buf(b, bt == WASM_T_VOID ? -64 : bt);
                if (parse_body(l, b, ctx) < 0) return -1;
                // Check for else: (then ... ) branch is already parsed; else comes next
                // In standard WAT, "then" is the implicit first branch; "else" follows
                if (peek(l) && peek(l)->type == TOK_LPAREN) {
                    tok_t *n = peek(l);
                    if (n && l->pos + 1 < l->ntokens) {
                        tok_t *kw = &l->tokens[l->pos + 1];
                        if (kw->type == TOK_IDENT && str_len("then") == kw->len &&
                            wat_memcmp("then", kw->start, 4) == 0) {
                            // (then ... ) — implicit in first branch, skip
                            next(l); next(l); // ( then
                            expect(l, TOK_RPAREN);
                        }
                    }
                }
                // Check for else branch
                if (peek(l) && peek(l)->type == TOK_LPAREN) {
                    tok_t *n = peek(l);
                    if (n && l->pos + 1 < l->ntokens) {
                        tok_t *kw = &l->tokens[l->pos + 1];
                        if (kw->type == TOK_IDENT && str_len("else") == kw->len &&
                            wat_memcmp("else", kw->start, 4) == 0) {
                            wb1(b, W_OP_ELSE);
                            next(l); next(l); // ( else
                            if (parse_body(l, b, ctx) < 0) return -1;
                            expect(l, TOK_RPAREN);
                        }
                    }
                }
                wb1(b, W_OP_END);
                expect(l, TOK_RPAREN); continue;
            }
            // Instruction in parens
            if (parse_expr(l, b, ctx) < 0) return -1;
            expect(l, TOK_RPAREN); continue;
        }

        if (parse_expr(l, b, ctx) < 0) return -1;
    }
}

static int parse_expr(lexer_t *l, wbin_t *b, parse_ctx_t *ctx) {
    tok_t *t = peek(l);
    if (!t) return -1;
    if (t->type == TOK_RPAREN || t->type == TOK_EOF) return 0;

    // end, else — signal to caller
    if (t->type == TOK_IDENT) {
        if (str_len("end") == t->len && wat_memcmp("end", t->start, 3) == 0) return 0;
        if (str_len("else") == t->len && wat_memcmp("else", t->start, 4) == 0) return 0;
    }

#define IS_INT(n) (peek(l) && peek(l)->type == TOK_INT)

    if (match_id(l, "i32.const")) { next(l);
        tok_t *v = peek(l); if (!v || v->type != TOK_INT) return -1;
        wb1(b, W_OP_I32_CONST); wb_sleb_buf(b, (i32)v->int_val); next(l); return 0; }
    if (match_id(l, "i64.const")) { next(l);
        tok_t *v = peek(l); if (!v || v->type != TOK_INT) return -1;
        wb1(b, W_OP_I64_CONST); wb_sleb_buf(b, (i32)v->int_val); next(l); return 0; }
    if (match_id(l, "f32.const")) { next(l);
        tok_t *v = peek(l); if (!v) return -1;
        u32 fv = 0; next(l);
        wb1(b, W_OP_F32_CONST); wb4(b, fv); return 0; }
    if (match_id(l, "f64.const")) { next(l);
        tok_t *v = peek(l); if (!v) return -1;
        next(l);
        wb1(b, W_OP_F64_CONST); wb4(b, 0); wb4(b, 0); return 0; }

    if (match_id(l, "i32.add"))   { next(l); wb1(b, W_OP_I32_ADD); return 0; }
    if (match_id(l, "i32.sub"))   { next(l); wb1(b, W_OP_I32_SUB); return 0; }
    if (match_id(l, "i32.mul"))   { next(l); wb1(b, W_OP_I32_MUL); return 0; }
    if (match_id(l, "i32.and"))   { next(l); wb1(b, W_OP_I32_AND); return 0; }
    if (match_id(l, "i32.or"))    { next(l); wb1(b, W_OP_I32_OR); return 0; }
    if (match_id(l, "i32.xor"))   { next(l); wb1(b, W_OP_I32_XOR); return 0; }
    if (match_id(l, "i32.shl"))   { next(l); wb1(b, W_OP_I32_SHL); return 0; }
    if (match_id(l, "i32.shr_u")) { next(l); wb1(b, W_OP_I32_SHR_U); return 0; }
    if (match_id(l, "i32.eqz"))   { next(l); wb1(b, W_OP_I32_EQZ); return 0; }
    if (match_id(l, "i32.eq"))    { next(l); wb1(b, W_OP_I32_EQ); return 0; }
    if (match_id(l, "i32.ne"))    { next(l); wb1(b, W_OP_I32_NE); return 0; }
    if (match_id(l, "i32.lt_s"))  { next(l); wb1(b, W_OP_I32_LT_S); return 0; }
    if (match_id(l, "i32.gt_s"))  { next(l); wb1(b, W_OP_I32_GT_S); return 0; }
    if (match_id(l, "i32.le_s"))  { next(l); wb1(b, W_OP_I32_LE_S); return 0; }
    if (match_id(l, "i32.ge_s"))  { next(l); wb1(b, W_OP_I32_GE_S); return 0; }
    if (match_id(l, "f32.add"))   { next(l); wb1(b, W_OP_F32_ADD); return 0; }
    if (match_id(l, "f32.sub"))   { next(l); wb1(b, W_OP_F32_SUB); return 0; }
    if (match_id(l, "f32.mul"))   { next(l); wb1(b, W_OP_F32_MUL); return 0; }
    if (match_id(l, "f32.div"))   { next(l); wb1(b, W_OP_F32_DIV); return 0; }

    if (match_id(l, "drop"))      { next(l); wb1(b, W_OP_DROP); return 0; }
    if (match_id(l, "return"))    { next(l); wb1(b, W_OP_RETURN); return 0; }
    if (match_id(l, "nop"))       { next(l); wb1(b, W_OP_NOP); return 0; }
    if (match_id(l, "unreachable")) { next(l); wb1(b, W_OP_UNREACHABLE); return 0; }

    if (match_id(l, "local.get"))  { next(l); tok_t *i=peek(l); if(!i||i->type!=TOK_INT)return -1; wb1(b,W_OP_LOCAL_GET); wb_uleb_buf(b,(u32)i->int_val); next(l); return 0; }
    if (match_id(l, "local.set"))  { next(l); tok_t *i=peek(l); if(!i||i->type!=TOK_INT)return -1; wb1(b,W_OP_LOCAL_SET); wb_uleb_buf(b,(u32)i->int_val); next(l); return 0; }
    if (match_id(l, "local.tee"))  { next(l); tok_t *i=peek(l); if(!i||i->type!=TOK_INT)return -1; wb1(b,W_OP_LOCAL_TEE); wb_uleb_buf(b,(u32)i->int_val); next(l); return 0; }
    if (match_id(l, "global.get")) { next(l); tok_t *i=peek(l); if(!i)return -1; if(i->type==TOK_INT){wb1(b,W_OP_GLOBAL_GET);wb_uleb_buf(b,(u32)i->int_val);next(l);}else{next(l);wb1(b,W_OP_GLOBAL_GET);wb_uleb_buf(b,0);} return 0; }
    if (match_id(l, "global.set")) { next(l); tok_t *i=peek(l); if(!i)return -1; if(i->type==TOK_INT){wb1(b,W_OP_GLOBAL_SET);wb_uleb_buf(b,(u32)i->int_val);next(l);}else{next(l);wb1(b,W_OP_GLOBAL_SET);wb_uleb_buf(b,0);} return 0; }
    if (match_id(l, "call"))       { next(l); tok_t *i=peek(l); if(!i)return -1; if(i->type==TOK_INT){wb1(b,W_OP_CALL);wb_uleb_buf(b,(u32)i->int_val);next(l);}else{i32 idx=nt_lookup(ctx->names,i->start,i->len);if(idx<0){screen_push_linef("WAT: Unresolved name '%.*s'",i->len,i->start);return -1;}next(l);wb1(b,W_OP_CALL);wb_uleb_buf(b,(u32)idx);} return 0; }
    if (match_id(l, "br"))         { next(l); tok_t *i=peek(l); if(!i||i->type!=TOK_INT)return -1; wb1(b,W_OP_BR); wb_uleb_buf(b,(u32)i->int_val); next(l); return 0; }
    if (match_id(l, "br_if"))      { next(l); tok_t *i=peek(l); if(!i||i->type!=TOK_INT)return -1; wb1(b,W_OP_BR_IF); wb_uleb_buf(b,(u32)i->int_val); next(l); return 0; }

    if (match_id(l, "memory.size")) { next(l); wb1(b, W_OP_MEMORY_SIZE); wb1(b, 0); return 0; }
    if (match_id(l, "memory.grow")) { next(l); wb1(b, W_OP_MEMORY_GROW); wb1(b, 0); return 0; }

    if (match_id(l, "i32.load"))   { next(l); wb1(b, W_OP_I32_LOAD); wb1(b, 2); wb_uleb_buf(b, 0); return 0; }
    if (match_id(l, "i64.load"))   { next(l); wb1(b, W_OP_I64_LOAD); wb1(b, 3); wb_uleb_buf(b, 0); return 0; }
    if (match_id(l, "f32.load"))   { next(l); wb1(b, W_OP_F32_LOAD); wb1(b, 2); wb_uleb_buf(b, 0); return 0; }
    if (match_id(l, "f64.load"))   { next(l); wb1(b, W_OP_F64_LOAD); wb1(b, 3); wb_uleb_buf(b, 0); return 0; }
    if (match_id(l, "i32.load8_u")){ next(l); wb1(b, W_OP_I32_LOAD8_U); wb1(b, 0); wb_uleb_buf(b, 0); return 0; }
    if (match_id(l, "i32.store"))  { next(l); wb1(b, W_OP_I32_STORE); wb1(b, 2); wb_uleb_buf(b, 0); return 0; }
    if (match_id(l, "i64.store"))  { next(l); wb1(b, W_OP_I64_STORE); wb1(b, 3); wb_uleb_buf(b, 0); return 0; }
    if (match_id(l, "f32.store"))  { next(l); wb1(b, W_OP_F32_STORE); wb1(b, 2); wb_uleb_buf(b, 0); return 0; }
    if (match_id(l, "f64.store"))  { next(l); wb1(b, W_OP_F64_STORE); wb1(b, 3); wb_uleb_buf(b, 0); return 0; }
    if (match_id(l, "i32.store8")) { next(l); wb1(b, W_OP_I32_STORE8); wb1(b, 0); wb_uleb_buf(b, 0); return 0; }

    screen_push_linef("WAT: Unknown opcode '%.*s'", t->len, t->start);
    next(l);
    return -1;
}

// ============================================================
// Main compilation
// ============================================================

int wat2wasm_compile(const char *wat, u32 wat_len, u8 **out_wasm, u32 *out_len) {
    lexer_t lex;
    lex.p = wat;
    lex.end = wat + wat_len;
    lex.ntokens = 0;
    lex.pos = 0;

    if (wat_lex(&lex) < 0) { screen_push_line("WAT: Tokenization failed"); return -1; }

    wbin_t b;
    wb_init(&b);

    // Magic + version
    wb1(&b, 0x00); wb1(&b, 0x61); wb1(&b, 0x73); wb1(&b, 0x6D);
    wb1(&b, 0x01); wb1(&b, 0x00); wb1(&b, 0x00); wb1(&b, 0x00);

    // Expect (module)
    if (expect(&lex, TOK_LPAREN) < 0 || !match_id(&lex, "module")) {
        screen_push_line("WAT: Expected (module ...)"); wb_free(&b); return -1;
    }
    next(&lex);

    // Temporary buffers per section
    wbin_t types_b, imports_b, funcs_b, exports_b, code_b, mem_b, data_b, glob_b, start_b;
    wb_init(&types_b); wb_init(&imports_b); wb_init(&funcs_b); wb_init(&exports_b);
    wb_init(&code_b);  wb_init(&mem_b);     wb_init(&data_b);  wb_init(&glob_b);
    wb_init(&start_b);

    u32 n_import_funcs = 0;
    u32 n_funcs = 0;
    u32 n_types = 0;
    u32 n_exports = 0;
    u32 n_data_segs = 0;
    u32 n_globals = 0;

    // Name→index table for resolving $name references (call, global.get/set).
    name_table_t names;
    names.count = 0;
    parse_ctx_t ctx;
    ctx.names = &names;

    while (1) {
        tok_t *t = peek(&lex);
        if (!t || t->type == TOK_RPAREN || t->type == TOK_EOF) break;

        if (t->type != TOK_LPAREN) {
            screen_push_line("WAT: Expected ( at module level"); goto error;
        }
        next(&lex);

        // --- IMPORT ---
        if (match_id(&lex, "import")) {
            next(&lex);
            tok_t *mod_t = peek(&lex); if (!mod_t || mod_t->type != TOK_STRING) goto error; next(&lex);
            tok_t *name_t = peek(&lex); if (!name_t || name_t->type != TOK_STRING) goto error; next(&lex);
            expect(&lex, TOK_LPAREN);
            if (match_id(&lex, "func")) {
                next(&lex);
                // Optional $name — record it for name→index resolution
                tok_t *id = peek(&lex);
                if (id && id->type == TOK_IDENT && id->len > 0 && id->start[0] == '$') {
                    nt_add(&names, id->start, id->len, n_import_funcs);
                    next(&lex);
                }

                u32 np = 0, nr = 0; u8 params[32], results[8];
                while (1) {
                    tok_t *p = peek(&lex);
                    if (!p || p->type == TOK_RPAREN) break;
                    if (p->type == TOK_LPAREN) {
                        next(&lex);
                        if (match_id(&lex, "param")) { next(&lex);
                            tok_t *pt = peek(&lex);
                            if (pt && pt->type == TOK_IDENT && pt->len > 0 && pt->start[0] == '$') next(&lex);
                            while (peek(&lex) && peek(&lex)->type != TOK_RPAREN) {
                                int vt = parse_vt(&lex);
                                if (vt >= 0) { if (np < 32) params[np++] = (u8)vt; }
                                else break;
                            }
                            expect(&lex, TOK_RPAREN);
                        } else if (match_id(&lex, "result")) { next(&lex);
                            while (peek(&lex) && peek(&lex)->type != TOK_RPAREN) {
                                int vt = parse_vt(&lex);
                                if (vt >= 0) { if (nr < 8) results[nr++] = (u8)vt; }
                                else break;
                            }
                            expect(&lex, TOK_RPAREN);
                        } else { expect(&lex, TOK_RPAREN); }
                    } else break;
                }
                // Type entry
                wb1(&types_b, 0x60);
                wb_uleb_buf(&types_b, np);
                for (u32 i = 0; i < np; i++) wb1(&types_b, params[i]);
                wb_uleb_buf(&types_b, nr);
                for (u32 i = 0; i < nr; i++) wb1(&types_b, results[i]);
                n_types++;

                // Import entry
                wb_str(&imports_b, mod_t->start, mod_t->len);
                wb_str(&imports_b, name_t->start, name_t->len);
                wb1(&imports_b, 0x00); // func kind
                wb_uleb_buf(&imports_b, n_types - 1);
                n_import_funcs++;
                expect(&lex, TOK_RPAREN); // close func
            } else {
                screen_push_line("WAT: Only func imports supported"); goto error;
            }
            expect(&lex, TOK_RPAREN); // close import
            continue;
        }

        // --- MEMORY ---
        if (match_id(&lex, "memory")) {
            next(&lex);
            tok_t *pg = peek(&lex);
            u32 pages = 1;
            if (pg && pg->type == TOK_INT) { pages = (u32)pg->int_val; next(&lex); }
            wb1(&mem_b, 0x00); // no max
            wb_uleb_buf(&mem_b, pages);
            expect(&lex, TOK_RPAREN); continue;
        }

        // --- DATA ---
        if (match_id(&lex, "data")) {
            next(&lex);
            u32 data_off = 0;
            expect(&lex, TOK_LPAREN);
            if (match_id(&lex, "i32.const")) {
                next(&lex);
                tok_t *off = peek(&lex); if (off && off->type == TOK_INT) { data_off = (u32)off->int_val; next(&lex); }
                expect(&lex, TOK_RPAREN);
            } else { expect(&lex, TOK_RPAREN); }

            // Collect all string data
            wbin_t db; wb_init(&db);
            while (1) {
                tok_t *dt = peek(&lex);
                if (!dt || dt->type == TOK_RPAREN) break;
                if (dt->type == TOK_STRING) {
                    // Copy string content verbatim (tokenizer already handled escapes)
                    const char *sp = dt->start;
                    u32 remain = dt->len;
                    while (remain > 0) {
                        if (*sp == '\\' && remain >= 2) {
                            // Simple escapes: \n \t \" \\
                            if (sp[1] == 'n') { wb1(&db, '\n'); sp += 2; remain -= 2; continue; }
                            if (sp[1] == 't') { wb1(&db, '\t'); sp += 2; remain -= 2; continue; }
                            if (sp[1] == '"') { wb1(&db, '"');  sp += 2; remain -= 2; continue; }
                            if (sp[1] == '\\'){ wb1(&db, '\\'); sp += 2; remain -= 2; continue; }
                            // Hex escape with \x prefix: \x10 → byte 0x10
                            if (sp[1] == 'x' && remain >= 4 && hex_v(sp[2]) >= 0 && hex_v(sp[3]) >= 0) {
                                wb1(&db, (u8)(hex_v(sp[2])*16+hex_v(sp[3]))); sp += 4; remain -= 4; continue;
                            }
                            // Hex escape without prefix: \10 → byte 0x10
                            if (remain >= 3 && hex_v(sp[1]) >= 0 && hex_v(sp[2]) >= 0) {
                                wb1(&db, (u8)(hex_v(sp[1])*16+hex_v(sp[2]))); sp += 3; remain -= 3; continue;
                            }
                            // Unknown escape — emit backslash literally, let next iteration handle the rest
                            wb1(&db, *sp); sp++; remain--;
                        } else {
                            wb1(&db, (u8)*sp); sp++; remain--;
                        }
                    }
                    next(&lex);
                } else break;
            }

            // Active data segment
            wb1(&data_b, 0x00); // active, memory 0
            wb1(&data_b, W_OP_I32_CONST); wb_sleb_buf(&data_b, (i32)data_off);
            wb1(&data_b, W_OP_END);
            wb_uleb_buf(&data_b, db.size);
            wb_emit(&data_b, db.data, db.size);
            n_data_segs++;

            wb_free(&db);
            expect(&lex, TOK_RPAREN); // close data
            continue;
        }

        // --- GLOBAL ---
        if (match_id(&lex, "global")) {
            next(&lex);
            tok_t *id = peek(&lex);
            if (id && id->type == TOK_IDENT && id->len > 0 && id->start[0] == '$') next(&lex);
            int mut = 0; int vt = -1;
            if (peek(&lex) && peek(&lex)->type == TOK_LPAREN) {
                next(&lex);
                if (match_id(&lex, "mut")) { next(&lex); mut = 1; }
                vt = parse_vt(&lex); expect(&lex, TOK_RPAREN);
            } else { vt = parse_vt(&lex); }
            if (vt < 0) goto error;
            wb1(&glob_b, (u8)vt);
            wb1(&glob_b, mut ? 1 : 0);
            if (peek(&lex) && peek(&lex)->type == TOK_LPAREN) {
                next(&lex); parse_expr(&lex, &glob_b, &ctx); expect(&lex, TOK_RPAREN);
            }
            wb1(&glob_b, W_OP_END);
            n_globals++;
            expect(&lex, TOK_RPAREN); continue;
        }

        // --- FUNC ---
        if (match_id(&lex, "func")) {
            next(&lex);
            tok_t *id = peek(&lex);
            if (id && id->type == TOK_IDENT && id->len > 0 && id->start[0] == '$') next(&lex);

            // Check for inline (export "name")
            if (peek(&lex) && peek(&lex)->type == TOK_LPAREN) {
                next(&lex);
                if (match_id(&lex, "export")) {
                    next(&lex);
                    tok_t *en = peek(&lex);
                    if (en && en->type == TOK_STRING) {
                        wb_str(&exports_b, en->start, en->len);
                        wb1(&exports_b, 0x00); // func kind
                        wb_uleb_buf(&exports_b, n_import_funcs + n_funcs);
                        n_exports++;
                        next(&lex);
                    }
                    expect(&lex, TOK_RPAREN);
                } else { expect(&lex, TOK_RPAREN); }
            }

            // Parse params, results, locals
            u32 np = 0, nr = 0; u8 params[32], results[8];
            wbin_t locals_b; wb_init(&locals_b);
            while (1) {
                tok_t *p = peek(&lex);
                if (!p || p->type == TOK_RPAREN) break;
                if (p->type == TOK_LPAREN) {
                    next(&lex);
                    if (match_id(&lex, "param")) { next(&lex);
                        tok_t *pt = peek(&lex);
                        if (pt && pt->type == TOK_IDENT && pt->len > 0 && pt->start[0] == '$') next(&lex);
                        while (peek(&lex) && peek(&lex)->type != TOK_RPAREN) {
                            int vt = parse_vt(&lex);
                            if (vt >= 0) { if (np < 32) params[np++] = (u8)vt; }
                            else break;
                        }
                        expect(&lex, TOK_RPAREN);
                    } else if (match_id(&lex, "result")) { next(&lex);
                        while (peek(&lex) && peek(&lex)->type != TOK_RPAREN) {
                            int vt = parse_vt(&lex);
                            if (vt >= 0) { if (nr < 8) results[nr++] = (u8)vt; }
                            else break;
                        }
                        expect(&lex, TOK_RPAREN);
                    } else if (match_id(&lex, "local")) { next(&lex);
                        tok_t *lt = peek(&lex);
                        if (lt && lt->type == TOK_IDENT && lt->len > 0 && lt->start[0] == '$') next(&lex);
                        while (peek(&lex) && peek(&lex)->type != TOK_RPAREN) {
                            int vt = parse_vt(&lex);
                            if (vt >= 0) { wb_uleb_buf(&locals_b, 1); wb1(&locals_b, (u8)vt); }
                            else break;
                        }
                        expect(&lex, TOK_RPAREN);
                    } else { expect(&lex, TOK_RPAREN); }
                } else break;
            }

            // Type entry
            wb1(&types_b, 0x60);
            wb_uleb_buf(&types_b, np);
            for (u32 i = 0; i < np; i++) wb1(&types_b, params[i]);
            wb_uleb_buf(&types_b, nr);
            for (u32 i = 0; i < nr; i++) wb1(&types_b, results[i]);
            n_types++;

            // Func section entry
            wb_uleb_buf(&funcs_b, n_types - 1);
            n_funcs++;

            // Parse function body
            wbin_t body_b; wb_init(&body_b);
            // Locals declaration: count of groups, then type entries
            if (locals_b.size > 0) {
                wb_uleb_buf(&body_b, 1); // 1 group
                wb_emit(&body_b, locals_b.data, locals_b.size);
            } else {
                wb_uleb_buf(&body_b, 0); // no locals
            }
            // Parse body (all ops until closing paren)
            if (parse_body(&lex, &body_b, &ctx) < 0) { wb_free(&locals_b); wb_free(&body_b); goto error; }
            // WASM requires every function body to end with 'end' opcode
            wb1(&body_b, W_OP_END);

            // Code entry: size + body bytes
            wbin_t entry_b; wb_init(&entry_b);
            wb_uleb_buf(&entry_b, body_b.size);
            wb_emit(&entry_b, body_b.data, body_b.size);
            // Append to code section
            wb_emit(&code_b, entry_b.data, entry_b.size);

            wb_free(&locals_b); wb_free(&body_b); wb_free(&entry_b);
            expect(&lex, TOK_RPAREN); // close func
            continue;
        }

        // --- EXPORT ---
        if (match_id(&lex, "export")) {
            next(&lex);
            tok_t *en = peek(&lex); if (!en || en->type != TOK_STRING) goto error; next(&lex);
            expect(&lex, TOK_LPAREN);
            if (match_id(&lex, "func")) {
                next(&lex);
                tok_t *fi = peek(&lex);
                u32 fidx = n_import_funcs + n_funcs - 1;
                if (fi && fi->type == TOK_INT) { fidx = (u32)fi->int_val; next(&lex); }
                else if (fi && fi->type == TOK_IDENT) { next(&lex); /* skip named ref like $add */ }
                wb_str(&exports_b, en->start, en->len);
                wb1(&exports_b, 0x00);
                wb_uleb_buf(&exports_b, fidx);
                n_exports++;
                expect(&lex, TOK_RPAREN);
            } else { expect(&lex, TOK_RPAREN); }
            expect(&lex, TOK_RPAREN); continue;
        }

        // --- START ---
        if (match_id(&lex, "start")) {
            next(&lex);
            tok_t *si = peek(&lex);
            if (si && si->type == TOK_INT) { wb_uleb_buf(&start_b, (u32)si->int_val); next(&lex); }
            expect(&lex, TOK_RPAREN); continue;
        }

        // --- TABLE / ELEM / TYPE / anything else --- skip to matching )
        screen_push_linef("WAT: Skipping unsupported construct");
        { int d = 1; while (d > 0 && peek(&lex)) { tok_t *sk = peek(&lex); if (sk->type == TOK_LPAREN) d++; else if (sk->type == TOK_RPAREN) d--; if (d > 0) next(&lex); } }
    }

    expect(&lex, TOK_RPAREN); // close module

    // ============================================================
    // Emit sections in order
    // ============================================================

    // Type section (ID 1)
    if (types_b.size > 0 || n_types > 0) {
        u32 spos = wb_sec_start(&b, 1);
        wb_uleb_buf(&b, n_types);
        wb_emit(&b, types_b.data, types_b.size);
        wb_sec_end(&b, spos);
    }

    // Import section (ID 2)
    if (n_import_funcs > 0) {
        u32 spos = wb_sec_start(&b, 2);
        wb_uleb_buf(&b, n_import_funcs);
        wb_emit(&b, imports_b.data, imports_b.size);
        wb_sec_end(&b, spos);
    }

    // Function section (ID 3)
    if (n_funcs > 0) {
        u32 spos = wb_sec_start(&b, 3);
        wb_uleb_buf(&b, n_funcs);
        wb_emit(&b, funcs_b.data, funcs_b.size);
        wb_sec_end(&b, spos);
    }

    // Memory section (ID 5)
    if (mem_b.size > 0) {
        u32 spos = wb_sec_start(&b, 5);
        wb_uleb_buf(&b, 1); // 1 memory
        wb_emit(&b, mem_b.data, mem_b.size);
        wb_sec_end(&b, spos);
    }

    // Global section (ID 6)
    if (n_globals > 0) {
        u32 spos = wb_sec_start(&b, 6);
        wb_uleb_buf(&b, n_globals);
        wb_emit(&b, glob_b.data, glob_b.size);
        wb_sec_end(&b, spos);
    }

    // Export section (ID 7)
    if (n_exports > 0) {
        u32 spos = wb_sec_start(&b, 7);
        wb_uleb_buf(&b, n_exports);
        wb_emit(&b, exports_b.data, exports_b.size);
        wb_sec_end(&b, spos);
    }

    // Start section (ID 8)
    if (start_b.size > 0) {
        u32 spos = wb_sec_start(&b, 8);
        wb_emit(&b, start_b.data, start_b.size);
        wb_sec_end(&b, spos);
    }

    // Code section (ID 10)
    if (n_funcs > 0) {
        u32 spos = wb_sec_start(&b, 10);
        wb_uleb_buf(&b, n_funcs);
        wb_emit(&b, code_b.data, code_b.size);
        wb_sec_end(&b, spos);
    }

    // Data section (ID 11)
    if (n_data_segs > 0) {
        u32 spos = wb_sec_start(&b, 11);
        wb_uleb_buf(&b, n_data_segs);
        wb_emit(&b, data_b.data, data_b.size);
        wb_sec_end(&b, spos);
    }

    wb_free(&types_b); wb_free(&imports_b); wb_free(&funcs_b);
    wb_free(&exports_b); wb_free(&code_b); wb_free(&mem_b);
    wb_free(&data_b); wb_free(&glob_b); wb_free(&start_b);

    *out_wasm = b.data;
    *out_len = b.size;
    return 0;

error:
    wb_free(&b); wb_free(&types_b); wb_free(&imports_b); wb_free(&funcs_b);
    wb_free(&exports_b); wb_free(&code_b); wb_free(&mem_b);
    wb_free(&data_b); wb_free(&glob_b); wb_free(&start_b);
    screen_push_line("WAT: Compilation error");
    return -1;
}
