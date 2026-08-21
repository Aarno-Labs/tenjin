#include <stdio.h>

/* A file-scope pointer and a local derived from it. Both are rewritten,
 * but in separate batches — each batch decides on its own which of its
 * members transformed — so their indices cannot be paired. The local's
 * initializer rebuilds the global's position instead, which is correct
 * either way and is what keeps the two batches' edits from colliding. */

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
