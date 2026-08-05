#include <stdio.h>

/* `p` never moves, so it is not an iterating pointer and must produce no
 * edits at all under either scheme. Guards against the frozen-handle path
 * widening the transform to pointers that gain nothing from it. */
static int peek(const int *buf, int n) {
    const int *p = buf;
    if (n <= 0)
        return 0;
    return p[0] + p[n - 1];
}

int main(void) {
    int d[4] = {2, 4, 6, 8};
    printf("%d\n", peek(d, 4));
    return 0;
}
