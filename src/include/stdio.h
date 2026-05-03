#ifndef SANDFLEA_STDIO_H
#define SANDFLEA_STDIO_H

#include "dialect.h"

typedef struct {
    int dummy;
} FILE;

extern FILE* stdout;
extern FILE* stderr;

#include <stdarg.h>

int printf(const char* format, ...);
int fprintf(FILE* f, const char* format, ...);
int sprintf(char* str, const char* format, ...);
int snprintf(char* str, size_t size, const char* format, ...);
int vsnprintf(char *str, size_t size, const char *format, va_list ap);
int puts(const char* s);
int putchar(int c);
int putc(int c, FILE* f);
int fputc(int c, FILE* f);
int fputs(const char* s, FILE* f);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* f);
int fflush(FILE* f);

#endif
