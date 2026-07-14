#ifndef _STDDEF_H_
#define _STDDEF_H_

#include "dialect.h"  /* already defines size_t, NULL */

typedef i64 ptrdiff_t;
typedef u32 wchar_t;

#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

#endif /* _STDDEF_H_ */
