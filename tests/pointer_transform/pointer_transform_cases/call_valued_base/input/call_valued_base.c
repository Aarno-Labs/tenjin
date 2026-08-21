#include <stdio.h>
#include <string.h>

/* The base is a function's return value. Pasting its text at every
 * access would have called the function once per access, so it was
 * rejected; a retained pointer evaluates the call exactly once, where
 * the declaration already did. */
static char *make(void) {
    static char store[8];
    memcpy(store, "abcdefg", 8);
    return store;
}

static int walk(void) {
    char *p = make();
    int n = 0;
    while (*p) {
        n += *p - 'a';
        p++;
    }
    return n;
}

int main(void) {
    printf("%d\n", walk());
    return 0;
}
