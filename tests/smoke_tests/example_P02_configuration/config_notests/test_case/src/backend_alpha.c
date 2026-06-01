/* This file is only compiled when BACKEND=alpha */

#include <stdio.h>
#include "backend.h"

void backend_init(SKELETON_ctx_t *ctx) {
    for (int i = 0; i < BUFFER_WORDS; i++) {
        ctx->base[i] = (word_t)i;
    }
    ctx->flags = 0xA0;
}

void backend_describe(const SKELETON_ctx_t *ctx) {
    printf("backend: alpha\n");
    printf("words: %d x %d bytes = %d bytes total\n",
           BUFFER_WORDS, WORD_BYTES, BUFFER_BYTES);
    printf("base[0]: %llu\n", (unsigned long long)ctx->base[0]);
}