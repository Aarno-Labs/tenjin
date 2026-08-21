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
    int p_index_xj = 0;
    int n = 0;
    while (p[p_index_xj]) {
        n += p[p_index_xj] - 'a';
        p_index_xj++;
    }
    return n;
}

int main(void) {
    printf("%d\n", walk());
    return 0;
}
