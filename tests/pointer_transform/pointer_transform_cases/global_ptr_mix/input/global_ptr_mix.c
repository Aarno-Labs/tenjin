#include <stdio.h>

/* A local pointer derived from a file-scope pointer. */

static char store[8] = "abcdefg";
static char *cursor = store;

static int drain(void) {
    char *p = cursor + 1;
    int n = 0;
    while (*p) {
        n += *p - 'a';
        p++;
    }
    cursor++;
    return n;
}

int main(void) {
    printf("%d ", drain());
    printf("%d\n", drain());
    return 0;
}
