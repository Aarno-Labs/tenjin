#include <stdio.h>

/* A file-scope pointer and a local derived from it. Both are rewritten,
 * but in separate batches — each batch decides on its own which of its
 * members transformed — so their indices cannot be paired. The local's
 * initializer rebuilds the global's position instead, which is correct
 * either way and is what keeps the two batches' edits from colliding. */

static char store[8] = "abcdefg";
static char *cursor = store;
static int cursor_index_xj = 0;

static int drain(void) {
    char *p = (cursor + cursor_index_xj) + 1;
    int p_index_xj = 0;
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
