#include <stdio.h>

/* A local pointer derived from a file-scope pointer. */

static char store[8] = "abcdefg";
static char *cursor = store;
static int cursor_index_xj = 0;

static int drain(void) {
    char *p = cursor;
    int p_index_xj = cursor_index_xj + 1;
    int n = 0;
    while (p[p_index_xj]) {
        n += p[p_index_xj] - 'a';
        p_index_xj++;
    }
    cursor_index_xj++;
    return n;
}

int main(void) {
    printf("%d ", drain());
    printf("%d\n", drain());
    return 0;
}
