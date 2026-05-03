#define STB_SPRINTF_IMPLEMENTATION
#include "../include/stbsupport.h"
#include "../include/string.h"
#include "../include/stdlib.h"
#include "../include/stdio.h"
#include "../include/time.h"
#include "../include/math.h"
#include "../include/kern_mem.h"
#include "../include/kern_vmm.h"
#include "../include/kern_serial.h"
#include "../util/util_str.h"
#include <stdarg.h>

int errno = 0;

// String.h
u0* memcpy(u0* dest, const u0* src, size_t n) {
    mem_move(dest, src, (u32)n);
    return dest;
}

u0* memset(u0* s, i32 c, size_t n) {
    mem_set(s, (u32)c, (u32)n);
    return s;
}

u0* memmove(u0* dest, const u0* src, size_t n) {
    return mem_move(dest, src, (u32)n);
}

i32 memcmp(const u0* s1, const u0* s2, size_t n) {
    const u8* p1 = s1;
    const u8* p2 = s2;
    while (n--) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++; p2++;
    }
    return 0;
}

size_t strlen(const char* s) {
    return str_len(s);
}

size_t strnlen(const char* s, size_t maxlen) {
    size_t i = 0;
    while (i < maxlen && s[i]) i++;
    return i;
}

i32 strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++; s2++;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

i32 strncmp(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++; s2++; n--;
    }
    if (n == 0) return 0;
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

char* strcat(char* dest, const char* src) {
    char* rd = dest;
    while (*dest) dest++;
    while ((*dest++ = *src++));
    return rd;
}

char* strcpy(char* dest, const char* src) {
    char* rd = dest;
    while ((*dest++ = *src++));
    return rd;
}

char* strncpy(char* dest, const char* src, size_t n) {
    char* rd = dest;
    while (n && (*dest++ = *src++)) n--;
    while (n--) *dest++ = 0;
    return rd;
}

char* strchr(const char* s, int c) {
    while (*s != (char)c) {
        if (!*s) return NULL;
        s++;
    }
    return (char*)s;
}

// Stdlib.h
u0* malloc(size_t size) {
    return kmalloc(size);
}

u0* calloc(size_t nmemb, size_t size) {
    return kmallocz(nmemb * size);
}

u0 free(u0* ptr) {
    kfree(ptr);
}

u0* realloc(u0* ptr, size_t size) {
    return (u0*)kern_realloc((void*)ptr, (u64)size);
}

u0 abort() {
    serial_outsl("ABORT CALLED");
    for(;;);
}

unsigned long long strtoull(const char *nptr, char **endptr, int base) {
    unsigned long long res = 0;
    const char *p = nptr;
    while (*p == ' ' || *p == '\t') p++;
    
    if (base == 0) {
        if (*p == '0') {
            if (p[1] == 'x' || p[1] == 'X') {
                base = 16;
                p += 2;
            } else {
                base = 8;
                p++;
            }
        } else {
            base = 10;
        }
    } else if (base == 16) {
        if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    }

    while (*p) {
        int v = -1;
        if (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'z') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') v = *p - 'A' + 10;
        
        if (v < 0 || v >= base) break;
        res = res * base + v;
        p++;
    }
    if (endptr) *endptr = (char *)p;
    return res;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    return (unsigned long)strtoull(nptr, endptr, base);
}

double strtod(const char *nptr, char **endptr) {
    double res = 0.0;
    const char *p = nptr;
    while (*p == ' ' || *p == '\t') p++;

    double sign = 1.0;
    if (*p == '-') {
        sign = -1.0;
        p++;
    } else if (*p == '+') {
        p++;
    }

    while (*p >= '0' && *p <= '9') {
        res = res * 10.0 + (*p - '0');
        p++;
    }

    if (*p == '.') {
        p++;
        double factor = 0.1;
        while (*p >= '0' && *p <= '9') {
            res += (*p - '0') * factor;
            factor *= 0.1;
            p++;
        }
    }

    if (endptr) *endptr = (char *)p;
    return res * sign;
}

// Time.h
extern volatile u64 sw;
clock_t clock() {
    return (clock_t)sw;
}

// Math.h
double sqrt(double x) { return __builtin_sqrt(x); }
float sqrtf(float x) { return __builtin_sqrtf(x); }
double floor(double x) { return __builtin_floor(x); }
float floorf(float x) { return __builtin_floorf(x); }
double ceil(double x) { return __builtin_ceil(x); }
float ceilf(float x) { return __builtin_ceilf(x); }
double fabs(double x) { return __builtin_fabs(x); }
float fabsf(float x) { return __builtin_fabsf(x); }
double trunc(double x) { return __builtin_trunc(x); }
float truncf(float x) { return __builtin_truncf(x); }
double rint(double x) { return __builtin_rint(x); }
float rintf(float x) { return __builtin_rintf(x); }
double copysign(double x, double y) { return __builtin_copysign(x, y); }
float copysignf(float x, float y) { return __builtin_copysignf(x, y); }


// Stdio.h
FILE* stdout = (FILE*)1;
FILE* stderr = (FILE*)2;

int printf(const char* format, ...) {
    char buf[1024];
    va_list args;
    va_start(args, format);
    int ret = stbsp_vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    serial_outs(buf);
    return ret;
}

int fprintf(FILE* f, const char* format, ...) {
    char buf[1024];
    va_list args;
    va_start(args, format);
    int ret = stbsp_vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    serial_outs(buf);
    return ret;
}

int sprintf(char* str, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int ret = stbsp_vsprintf(str, format, args);
    va_end(args);
    return ret;
}

int snprintf(char* str, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int ret = stbsp_vsnprintf(str, (int)size, format, args);
    va_end(args);
    return ret;
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    return stbsp_vsnprintf(str, (int)size, format, ap);
}

int puts(const char* s) {
    serial_outsl(s);
    return 0;
}

int putchar(int c) {
    char buf[2] = {(char)c, 0};
    serial_outs(buf);
    return c;
}

int putc(int c, FILE* f) {
    return putchar(c);
}

int fputc(int c, FILE* f) {
    return putchar(c);
}

int fputs(const char* s, FILE* f) {
    serial_outs(s);
    return 0;
}

size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* f) {
    size_t total = size * nmemb;
    const char* p = (const char*)ptr;
    for (size_t i = 0; i < total; i++) {
        putchar(p[i]);
    }
    return nmemb;
}

int fflush(FILE* f) {
    return 0;
}

char *strncat(char *restrict d, const char *restrict s, size_t n)
{
    char *a = d;
    d += strlen(d);
    while (n && *s) n--, *d++ = *s++;
    *d++ = 0;
    return a;
}
