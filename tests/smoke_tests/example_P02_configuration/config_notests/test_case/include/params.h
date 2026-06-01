#ifndef PARAMS_H
#define PARAMS_H

#include <stdint.h>

/* Token-pasting namespace macro */
#define SKELETON_NAMESPACE(s) SKELETON_##s

/* WORD_SIZE is injected via -DWORD_SIZE=32 (not a string) */
#ifndef WORD_SIZE
#error "WORD_SIZE must be defined via CMake (set WORD_SIZE cache variable)"
#endif

/* #if / #elif / #error: value-comparison on an injected integer token */
#if WORD_SIZE == 64
#  define WORD_BYTES 8
   typedef uint64_t word_t;
#elif WORD_SIZE == 32
#  define WORD_BYTES 4
   typedef uint32_t word_t;
#else
#error "WORD_SIZE must be 32 or 64"
#endif

#define BUFFER_WORDS 16
#define BUFFER_BYTES (BUFFER_WORDS * WORD_BYTES)
#define BUFFER_BITS  (BUFFER_BYTES * 8)

/* Conditional struct layout: the extended field only exists in the binary when ENABLE_EXTRA is defined */
typedef struct {
    word_t   base[BUFFER_WORDS];
    uint32_t flags;
#ifdef ENABLE_EXTRA
    word_t   extended[BUFFER_WORDS];
#endif
} SKELETON_NAMESPACE(ctx_t);    /* expands to: SKELETON_ctx_t */

#endif /* PARAMS_H */