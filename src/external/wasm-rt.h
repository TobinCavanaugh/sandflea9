/* Adapted wasm2c runtime header for sandfleaOS (freestanding kernel).
 * Strips out pthreads, mmap, signal handlers, threads, and segue.
 * Memory: kmalloc/kfree via our stdlib.h. Traps: setjmp/longjmp. */
#ifndef WASM_RT_H_
#define WASM_RT_H_

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* No builtin detection needed — we use plain memcpy */
#define wasm_rt_memcpy memcpy
#define wasm_rt_unreachable abort

/* Disable all advanced features */
#define WASM_RT_USE_MMAP                0
#define WASM_RT_MEMCHECK_BOUNDS_CHECK   1
#define WASM_RT_MEMCHECK_GUARD_PAGES    0
#define WASM_RT_SKIP_SIGNAL_RECOVERY    1
#define WASM_RT_INSTALL_SIGNAL_HANDLER  0
#define WASM_RT_ALLOW_SEGUE             0
#define WASM_RT_USE_SEGUE               0
#define WASM_RT_NONCONFORMING_UNCHECKED_STACK_EXHAUSTION 1
#define WASM_RT_STACK_DEPTH_COUNT       0
#define WASM_RT_STACK_EXHAUSTION_HANDLER 0
#define WASM_RT_SANITY_CHECKS           0

#define WASM_RT_NO_RETURN __attribute__((noreturn))

#if __has_builtin(__builtin_expect)
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x)   (x)
#define UNLIKELY(x) (x)
#endif

/* Value types — must match what the generated code expects */
typedef enum {
  WASM_RT_I32,
  WASM_RT_I64,
  WASM_RT_F32,
  WASM_RT_F64,
  WASM_RT_V128,
  WASM_RT_FUNCREF,
  WASM_RT_EXTERNREF,
} wasm_rt_type_t;

/* Trap codes */
typedef enum {
  WASM_RT_TRAP_NONE,
  WASM_RT_TRAP_OOB,
  WASM_RT_TRAP_INT_OVERFLOW,
  WASM_RT_TRAP_DIV_BY_ZERO,
  WASM_RT_TRAP_INVALID_CONVERSION,
  WASM_RT_TRAP_UNREACHABLE,
  WASM_RT_TRAP_CALL_INDIRECT,
  WASM_RT_TRAP_NULL_REF,
  WASM_RT_TRAP_UNCAUGHT_EXCEPTION,
  WASM_RT_TRAP_UNALIGNED,
  WASM_RT_TRAP_EXHAUSTION,
} wasm_rt_trap_t;

/* Simple jmp_buf wrapper — our setjmp.h has plain jmp_buf (array of longs) */
typedef struct {
  bool initialized;
  jmp_buf buffer;
} wasm_rt_jmp_buf;

#define WASM_RT_SETJMP(buf) \
  ((buf).initialized = true, setjmp((buf).buffer))

#define WASM_RT_LONGJMP(buf, val) \
  do {                                \
    if (!((buf).initialized)) abort();\
    (buf).initialized = false;        \
    longjmp((buf).buffer, val);       \
  } while(0)

/* Function types */
typedef void (*wasm_rt_function_ptr_t)(void);

typedef struct wasm_rt_tailcallee_t {
  void (*fn)(void** instance_ptr,
             void* tail_call_stack,
             struct wasm_rt_tailcallee_t* next);
} wasm_rt_tailcallee_t;

typedef const char* wasm_rt_func_type_t;

typedef struct {
  wasm_rt_func_type_t   func_type;
  wasm_rt_function_ptr_t func;
  wasm_rt_tailcallee_t  func_tailcallee;
  void*                  module_instance;
} wasm_rt_funcref_t;

#define wasm_rt_funcref_null_value \
  ((wasm_rt_funcref_t){NULL, NULL, {NULL}, NULL})

/* externref */
typedef void* wasm_rt_externref_t;
#define wasm_rt_externref_null_value ((wasm_rt_externref_t){NULL})

/* Memory */
typedef struct {
  uint8_t* data;
  uint64_t size;
  uint64_t pages;
  uint64_t max_pages;
  uint32_t page_size;
  bool     is64;
} wasm_rt_memory_t;

#define WASM_DEFAULT_PAGE_SIZE 65536

/* Tables */
typedef struct {
  wasm_rt_funcref_t* data;
  uint32_t max_size;
  uint32_t size;
} wasm_rt_funcref_table_t;

typedef struct {
  wasm_rt_externref_t* data;
  uint32_t max_size;
  uint32_t size;
} wasm_rt_externref_table_t;

/* Runtime API */
WASM_RT_NO_RETURN void wasm_rt_trap(wasm_rt_trap_t);
const char* wasm_rt_strerror(wasm_rt_trap_t trap);

void wasm_rt_allocate_memory(wasm_rt_memory_t*, uint64_t initial_pages,
                             uint64_t max_pages, bool is64, uint32_t page_size);
uint64_t wasm_rt_grow_memory(wasm_rt_memory_t*, uint64_t pages);
void wasm_rt_free_memory(wasm_rt_memory_t*);

void wasm_rt_allocate_funcref_table(wasm_rt_funcref_table_t*, uint32_t elements, uint32_t max_elements);
void wasm_rt_free_funcref_table(wasm_rt_funcref_table_t*);
uint32_t wasm_rt_grow_funcref_table(wasm_rt_funcref_table_t*, uint32_t delta, wasm_rt_funcref_t init);

void wasm_rt_allocate_externref_table(wasm_rt_externref_table_t*, uint32_t elements, uint32_t max_elements);
void wasm_rt_free_externref_table(wasm_rt_externref_table_t*);
uint32_t wasm_rt_grow_externref_table(wasm_rt_externref_table_t*, uint32_t delta, wasm_rt_externref_t init);

/* wasm_rt_impl_try — saved in impl */
extern wasm_rt_jmp_buf g_wasm_rt_jmp_buf;
#define WASM_RT_SAVE_STACK_DEPTH() (void)0
#define wasm_rt_impl_try()        WASM_RT_SETJMP(g_wasm_rt_jmp_buf)

#ifdef __cplusplus
}
#endif

#endif
