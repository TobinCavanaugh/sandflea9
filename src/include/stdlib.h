#ifndef SANDFLEA_STDLIB_H
#define SANDFLEA_STDLIB_H

#include "dialect.h"

u0* malloc(size_t size);
u0* calloc(size_t nmemb, size_t size);
u0 free(u0* ptr);
u0* realloc(u0* ptr, size_t size);
u0 abort();

unsigned long strtoul(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
double strtod(const char *nptr, char **endptr);

#endif
