/* wasm2c runtime implementation for sandfleaOS (freestanding kernel).
 * Memory: kmalloc/kfree via stdlib.h. Traps: setjmp/longjmp.
 * No mmap, no threads, no signal handlers. */

#include "wasm-rt.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Global jmp_buf for trap handling */
wasm_rt_jmp_buf g_wasm_rt_jmp_buf;

void wasm_rt_trap(wasm_rt_trap_t code) {
  assert(code != WASM_RT_TRAP_NONE);
  WASM_RT_LONGJMP(g_wasm_rt_jmp_buf, code);
}

void wasm_rt_allocate_memory(wasm_rt_memory_t* mem,
                             uint64_t initial_pages,
                             uint64_t max_pages,
                             bool is64,
                             uint32_t page_size) {
  mem->page_size = page_size ? page_size : WASM_DEFAULT_PAGE_SIZE;
  mem->pages     = initial_pages;
  mem->max_pages = max_pages ? max_pages : 0x10000;
  mem->is64      = is64;
  mem->size      = initial_pages * mem->page_size;
  mem->data      = (uint8_t*)calloc(mem->size, 1);
  if (!mem->data) {
    mem->size = 0;
    mem->pages = 0;
  }
}

uint64_t wasm_rt_grow_memory(wasm_rt_memory_t* mem, uint64_t pages) {
  if (pages == 0) return mem->pages;
  uint64_t new_pages = mem->pages + pages;
  if (new_pages > mem->max_pages) return 0xFFFFFFFFu;
  uint64_t new_size = new_pages * mem->page_size;
  uint8_t* new_data = (uint8_t*)realloc(mem->data, new_size);
  if (!new_data) return 0xFFFFFFFFu;
  memset(new_data + mem->size, 0, new_size - mem->size);
  mem->data = new_data;
  mem->size = new_size;
  uint64_t old = mem->pages;
  mem->pages = new_pages;
  return old;
}

void wasm_rt_free_memory(wasm_rt_memory_t* mem) {
  if (mem->data) { free(mem->data); mem->data = NULL; }
  mem->size = 0;
  mem->pages = 0;
}

/* funcref table */
void wasm_rt_allocate_funcref_table(wasm_rt_funcref_table_t* tbl,
                                    uint32_t elements, uint32_t max_elements) {
  tbl->size     = elements;
  tbl->max_size = max_elements;
  tbl->data     = (wasm_rt_funcref_t*)calloc(elements, sizeof(wasm_rt_funcref_t));
}

void wasm_rt_free_funcref_table(wasm_rt_funcref_table_t* tbl) {
  if (tbl->data) { free(tbl->data); tbl->data = NULL; }
  tbl->size = 0;
}

uint32_t wasm_rt_grow_funcref_table(wasm_rt_funcref_table_t* tbl,
                                    uint32_t delta, wasm_rt_funcref_t init) {
  uint32_t old = tbl->size;
  uint32_t new_size = old + delta;
  if (new_size < old || (tbl->max_size && new_size > tbl->max_size))
    return 0xFFFFFFFFu;
  wasm_rt_funcref_t* new_data = (wasm_rt_funcref_t*)realloc(tbl->data,
      new_size * sizeof(wasm_rt_funcref_t));
  if (!new_data) return 0xFFFFFFFFu;
  for (uint32_t i = old; i < new_size; i++) new_data[i] = init;
  tbl->data = new_data;
  tbl->size = new_size;
  return old;
}

/* externref table */
void wasm_rt_allocate_externref_table(wasm_rt_externref_table_t* tbl,
                                      uint32_t elements, uint32_t max_elements) {
  tbl->size     = elements;
  tbl->max_size = max_elements;
  tbl->data     = (wasm_rt_externref_t*)calloc(elements, sizeof(wasm_rt_externref_t));
}

void wasm_rt_free_externref_table(wasm_rt_externref_table_t* tbl) {
  if (tbl->data) { free(tbl->data); tbl->data = NULL; }
  tbl->size = 0;
}

uint32_t wasm_rt_grow_externref_table(wasm_rt_externref_table_t* tbl,
                                      uint32_t delta, wasm_rt_externref_t init) {
  uint32_t old = tbl->size;
  uint32_t new_size = old + delta;
  if (new_size < old || (tbl->max_size && new_size > tbl->max_size))
    return 0xFFFFFFFFu;
  wasm_rt_externref_t* new_data = (wasm_rt_externref_t*)realloc(tbl->data,
      new_size * sizeof(wasm_rt_externref_t));
  if (!new_data) return 0xFFFFFFFFu;
  for (uint32_t i = old; i < new_size; i++) new_data[i] = init;
  tbl->data = new_data;
  tbl->size = new_size;
  return old;
}

const char* wasm_rt_strerror(wasm_rt_trap_t trap) {
  switch (trap) {
    case WASM_RT_TRAP_NONE:             return "No error";
    case WASM_RT_TRAP_OOB:              return "Out-of-bounds access";
    case WASM_RT_TRAP_INT_OVERFLOW:     return "Integer overflow";
    case WASM_RT_TRAP_DIV_BY_ZERO:      return "Divide by zero";
    case WASM_RT_TRAP_INVALID_CONVERSION: return "Invalid conversion";
    case WASM_RT_TRAP_UNREACHABLE:      return "Unreachable";
    case WASM_RT_TRAP_CALL_INDIRECT:    return "Invalid call_indirect";
    case WASM_RT_TRAP_NULL_REF:         return "Null reference";
    case WASM_RT_TRAP_UNCAUGHT_EXCEPTION: return "Uncaught exception";
    case WASM_RT_TRAP_UNALIGNED:        return "Unaligned atomic access";
    case WASM_RT_TRAP_EXHAUSTION:       return "Stack exhausted";
    default:                            return "Unknown trap";
  }
}
