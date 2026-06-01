/* This file is only compiled when BACKEND=beta */

#include <stdio.h>
#include "backend.h"

void backend_init(SKELETON_ctx_t *ctx) {
    for (int i = 0; i < BUFFER_WORDS; i++) {
        ctx->base[i] = (word_t)(i * 0xFF);
    }
    ctx->flags = 0xB0;
}

void backend_describe(const SKELETON_ctx_t *ctx) {
    printf("backend: beta\n");
    printf("words: %d x %d bytes = %d bytes total\n",
           BUFFER_WORDS, WORD_BYTES, BUFFER_BYTES);
    printf("base[0]: %llu\n", (unsigned long long)ctx->base[0]);
}