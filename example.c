#include <stdio.h>
#include <stdarg.h>

#define ARENA_IMPLEMENTATION
#define ARENA_ENABLE_DEBUG_LOG
#include "arena.h"


void allocate_normal(arena* a) {
  const size_t count = 10;
  int* arr = arena_alloc(a, count * sizeof(int));
  for (size_t i = 0; i < count; i++) {
    arr[i] = i;
  }
}

void allocate_oversized(arena* a) {
  const size_t count = 10 * 1024 * 1024;
  int* big_arr = arena_alloc(a, count * sizeof(int));
  for (size_t i = 0; i < count; i++) {
    big_arr[i] = i;
  }
}


int main(void) {
  arena a;
  arena_init_simple(&a, 1024*1024);

  allocate_normal(&a);
  allocate_oversized(&a);
  allocate_normal(&a);

  arena_destroy(&a);

  return 0;
}
