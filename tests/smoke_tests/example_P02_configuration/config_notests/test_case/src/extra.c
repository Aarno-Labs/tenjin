#include <stdio.h>
#include "extra.h"

void extra_run(SKELETON_ctx_t *ctx) {
    /* ctx->extended only exists in the struct layout when ENABLE_EXTRA is defined.
       This file is only compiled when ENABLE_EXTRA=ON, so the field is always present here */
    for (int i = 0; i < BUFFER_WORDS; i++) {
        ctx->extended[i] = ctx->base[i] ^ (word_t)0xCAFE;
    }
    printf("extra: enabled (%d extra bytes)\n",
           BUFFER_WORDS * WORD_BYTES);
}