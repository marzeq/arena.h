/*
------------------------------------------------------------------------------
arena.h — configurable chunked arena allocator


USAGE:

  #define ARENA_IMPLEMENTATION
  #include "arena.h"

  arena a;
  arena_init(&a);

  void* p = arena_alloc(&a, 128);
  void* q = arena_alloc(&a, 8000);

  arena_reset(&a);
  arena_destroy(&a);

CUSTOM CONFIGURATION:

  size_t sizes[] = { 4096, 32768, 262144 };
  arena_init_custom(&a, sizes, 3);

SIMPLE CONFIGURATION:

  arena_init_simple(&a, 65536); // single class with 64KB chunk size

DEFAULT CONFIGURATION (if not overridden):

  3 classes:
    { 8*1024, 64*1024, 256*1024 }

CONFIGURATION MACROS (optional):

  ARENA_DEFAULT_CLASS_COUNT
      Number of classes in the default configuration.
      Default: 3

  ARENA_DEFAULT_CLASS_SIZES
      Class chunk sizes in bytes in the default configuration (must be a multiple of ARENA_ALIGNMENT).
      Default: { 8*1024, 64*1024, 256*1024 }

  ARENA_ALIGNMENT
      Allocation alignment in bytes (must be a power of two).
      Default: 8

  ARENA_ENABLE_DEBUG_LOG
      Define to enable internal logging.

  ARENA_DEBUG_LOG_FN
      Custom logger function.
      Signature: void fn(const char* fmt, va_list args)

NOTES / DISCLAIMERS:

  - This allocator is NOT thread-safe.
    Concurrent access must be externally synchronised.

  - Allocated memory is NOT zero-initialised.

  - Allocations larger than the largest class threshold
    are placed in a dedicated oversized class.
    Oversized chunks are freed on arena_reset.

LICENSE:

  Copyright © 2026 Antoni Marzec

  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

------------------------------------------------------------------------------
*/

#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>
#include <stdint.h>

#ifndef ARENA_ALIGNMENT
#define ARENA_ALIGNMENT 8
#endif

#ifndef ARENA_DEFAULT_CLASS_COUNT
#define ARENA_DEFAULT_CLASS_COUNT 3
#endif

#ifndef ARENA_DEFAULT_CLASS_SIZES
#define ARENA_DEFAULT_CLASS_SIZES { 8 * 1024, 64 * 1024, 256 * 1024 }
#endif

typedef struct arena_chunk {
  uint8_t* memory;
  size_t capacity;
  size_t offset;
  struct arena_chunk* next;
} arena_chunk;

typedef struct {
  arena_chunk* head;
  arena_chunk* current;
  size_t chunk_size;
} arena_class;

typedef struct {
  arena_class* classes;
  size_t class_count;
  arena_class oversized;
  const size_t* class_sizes;
} arena;

// Initialise arena with default configuration.
int arena_init(arena* a);

// Initialise arena with custom configuration.
// class_sizes must be an array of class_count sizes in bytes (must be a multiple of ARENA_ALIGNMENT).
int arena_init_custom(
  arena* a,
  const size_t* class_sizes,
  size_t class_count
);

// Initialise arena with a single class of fixed chunk size.
int arena_init_simple(arena* a, size_t chunk_size);

// Allocate memory from the arena.
void* arena_alloc(arena* a, size_t size);

// Reset the arena state without freeing memory, with the exception of oversized chunks.
void arena_reset(arena* a);

// Free all memory associated with the arena.
void arena_destroy(arena* a);

#endif  // ARENA_H

#ifdef ARENA_IMPLEMENTATION

#include <stdlib.h>

_Static_assert(
  (ARENA_ALIGNMENT & (ARENA_ALIGNMENT - 1)) == 0,
  "ARENA_ALIGNMENT must be a power of two"
);

_Static_assert(
  ARENA_DEFAULT_CLASS_COUNT > 0,
  "ARENA_DEFAULT_CLASS_COUNT must be greater than zero"
);

#ifdef ARENA_ENABLE_DEBUG_LOG

#include <stdio.h>
#include <stdarg.h>

#ifndef ARENA_DEBUG_LOG_FN
static void arena_default_logger(const char* fmt, va_list args) {
  fprintf(stderr, "[arena] ");
  vfprintf(stderr, fmt, args);
}
#define ARENA_DEBUG_LOG_FN arena_default_logger
#endif

static void arena_log_internal(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ARENA_DEBUG_LOG_FN(fmt, args);
  va_end(args);
}

#define ARENA_LOG(...) arena_log_internal(__VA_ARGS__)

#else
#define ARENA_LOG(...) ((void)0)
#endif

static size_t arena_align(size_t x) {
  size_t mask = ARENA_ALIGNMENT - 1;
  return (x + mask) & ~mask;
}

static arena_chunk* arena_chunk_create(size_t capacity) {
  ARENA_LOG("chunk_create: capacity=%zu\n", capacity);

  arena_chunk* c = (arena_chunk*)malloc(sizeof(arena_chunk));
  if (c == NULL) {
    ARENA_LOG("chunk_create: metadata malloc failed\n");
    return NULL;
  }

  c->memory = (uint8_t*)malloc(capacity);
  if (c->memory == NULL) {
    ARENA_LOG("chunk_create: memory malloc failed (capacity=%zu)\n", capacity);
    free(c);
    return NULL;
  }

  c->capacity = capacity;
  c->offset = 0;
  c->next = NULL;

  ARENA_LOG("chunk_create: success ptr=%p\n", (void*)c->memory);
  return c;
}

static void arena_class_init(
  arena_class* cls,
  size_t chunk_size
) {
  ARENA_LOG("class_init: chunk_size=%zu (fixed)\n", chunk_size);

  cls->head = NULL;
  cls->current = NULL;
  cls->chunk_size = chunk_size;
}

static void arena_class_destroy(arena_class* cls) {
  arena_chunk* c = cls->head;
  while (c != NULL) {
    arena_chunk* next = c->next;
    free(c->memory);
    free(c);
    c = next;
  }
  cls->head = NULL;
  cls->current = NULL;
}

static void arena_class_reset(arena_class* cls) {
  for (arena_chunk* c = cls->head; c != NULL; c = c->next) {
    c->offset = 0;
  }
  cls->current = cls->head;
}

static void* arena_class_alloc(arena_class* cls, size_t size) {
  size = arena_align(size);

  ARENA_LOG(
    "class_alloc: request=%zu current=%p\n",
    size,
    (void*)cls->current
  );

  if (
    cls->current == NULL ||
    cls->current->offset + size > cls->current->capacity
  ) {
    size_t chunk_size = cls->chunk_size;

    if (size > chunk_size) {
      chunk_size = size;
    }

    ARENA_LOG(
      "class_alloc: new chunk required size=%zu chunk_size=%zu\n",
      size,
      chunk_size
    );

    arena_chunk* chunk = arena_chunk_create(chunk_size);
    if (chunk == NULL) {
      ARENA_LOG("class_alloc: chunk_create failed\n");
      return NULL;
    }

    if (cls->head == NULL) {
      cls->head = chunk;
    } else {
      cls->current->next = chunk;
    }

    cls->current = chunk;
  }

  void* ptr = cls->current->memory + cls->current->offset;
  cls->current->offset += size;

  ARENA_LOG(
    "class_alloc: return=%p new_offset=%zu capacity=%zu\n",
    ptr,
    cls->current->offset,
    cls->current->capacity
  );

  return ptr;
}

int arena_init_custom(
  arena* a,
  const size_t* class_sizes,
  size_t class_count
) {
  if (a == NULL || class_sizes == NULL || class_count == 0) {
    return 0;
  }

  a->class_count = class_count;
  a->class_sizes = class_sizes;

  a->classes = (arena_class*)malloc(sizeof(arena_class) * class_count);
  if (a->classes == NULL) {
    return 0;
  }

  for (size_t i = 0; i < class_count; ++i) {
    arena_class_init(&a->classes[i], class_sizes[i]);
  }

  arena_class_init(
    &a->oversized,
    class_sizes[class_count - 1]
  );

  return 1;
}

int arena_init(arena* a) {
  static const size_t defaults[ARENA_DEFAULT_CLASS_COUNT] =
    ARENA_DEFAULT_CLASS_SIZES;

  return arena_init_custom(
    a,
    defaults,
    sizeof(defaults) / sizeof(defaults[0])
  );
}

int arena_init_simple(arena* a, size_t chunk_size) {
  return arena_init_custom(
    a,
    &chunk_size,
    1
  );
}

void arena_reset(arena* a) {
  ARENA_LOG("arena_reset\n");

  for (size_t i = 0; i < a->class_count; ++i) {
    ARENA_LOG("arena_reset: class[%zu]\n", i);
    arena_class_reset(&a->classes[i]);
  }

  ARENA_LOG("arena_reset: destroy oversized\n");
  arena_class_destroy(&a->oversized);

  arena_class_init(
    &a->oversized,
    a->class_sizes[a->class_count - 1]
  );
}

void arena_destroy(arena* a) {
  ARENA_LOG("arena_destroy\n");

  for (size_t i = 0; i < a->class_count; ++i) {
    ARENA_LOG("arena_destroy: class[%zu]\n", i);
    arena_class_destroy(&a->classes[i]);
  }

  free(a->classes);
  arena_class_destroy(&a->oversized);
}

void* arena_alloc(arena* a, size_t size) {
  size = arena_align(size);

  ARENA_LOG("arena_alloc: size=%zu\n", size);

  for (size_t i = 0; i < a->class_count; ++i) {
    if (size <= a->class_sizes[i]) {
      ARENA_LOG(
        "arena_alloc: using class[%zu] threshold=%zu\n",
        i,
        a->class_sizes[i]
      );
      return arena_class_alloc(&a->classes[i], size);
    }
  }

  ARENA_LOG("arena_alloc: using oversized class\n");
  return arena_class_alloc(&a->oversized, size);
}

#endif  // ARENA_IMPLEMENTATION
