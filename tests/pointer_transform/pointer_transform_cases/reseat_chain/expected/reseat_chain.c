#include <stdio.h>

/* A chain of pointers derived from a reseated, moving pointer:
 * p1 = p0 + 1, p2 = p1 + 1, p3 = p2 + 1. Each link's position depends on
 * where its source actually is at that moment, not on where its source
 * started, so the chain has to carry the source's index through. */
static int chain(int *a, int *b, int n) {
    int *p0 = a;
    if (n < 0)
        p0 = b;
    p0++;
    int *p1 = p0 + 1;
    int p2_index_xj = 1;
    int p3_index_xj = p2_index_xj + (1);
    return *p0 + *p1 + p1[p2_index_xj] + p1[p3_index_xj];
}

int main(void) {
    int a[8] = {1, 2, 4, 8, 16, 32, 64, 128};
    int b[8] = {100, 200, 400, 800, 1600, 3200, 6400, 12800};
    printf("%d %d\n", chain(a, b, 1), chain(a, b, -1));
    return 0;
}
