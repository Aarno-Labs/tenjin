#include <stdio.h>
#include "backend.h"   /* gets params.h transitively via PUBLIC include path */

#ifndef APP_MODE_STR
#error "APP_MODE_STR must be defined via CMake"
#endif

/* Stringification is needed because BUILD_PROFILE is injected without quotes. */
#define STRINGIFY(x) #x
#define TOSTRING(x)  STRINGIFY(x)

#ifdef ENABLE_EXTRA
#  include "extra.h"
#endif

int main(void) {
    SKELETON_ctx_t ctx;

    printf("preset_skeleton_v2\n");
    printf("profile: %s\n", TOSTRING(BUILD_PROFILE)); /* bare token -> string */
    printf("mode: %s\n", APP_MODE_STR); /* already a string literal */

    backend_init(&ctx);
    backend_describe(&ctx);

#ifdef ENABLE_EXTRA
    extra_run(&ctx);
#else
    printf("extra: disabled\n");
#endif

    return 0;
}