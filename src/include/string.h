#ifndef SANDFLEA_STRING_H
#define SANDFLEA_STRING_H

#include "dialect.h"

u0* memcpy(u0* dest, const u0* src, size_t n);
u0* memset(u0* s, i32 c, size_t n);
u0* memmove(u0* dest, const u0* src, size_t n);
i32 memcmp(const u0* s1, const u0* s2, size_t n);
size_t strlen(const char* s);
size_t strnlen(const char* s, size_t maxlen);
i32 strcmp(const char* s1, const char* s2);
i32 strncmp(const char* s1, const char* s2, size_t n);
char* strcat(char* dest, const char* src);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, size_t n);
char* strchr(const char* s, int c);
char *strncat(char *restrict d, const char *restrict s, size_t n);

#endif
