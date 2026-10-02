#include <stdio.h>
#include <string.h>

/* The pointer is initialized from a function call. */
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
