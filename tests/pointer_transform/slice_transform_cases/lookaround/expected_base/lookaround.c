#include <stdio.h>

/* Exercises lookback (via *(p - 1)) and lookahead (via *(p + 1)): the
 * constructed slice at the call site must be widened on both sides. */
static int lookaround(const int *buf, int n) {
    int p_index_xj = 1;
    int i = 1;
    int total = 0;
    while (p_index_xj < n) {
        total += buf[p_index_xj - 1];
        if (i + 1 < n)
            total += buf[p_index_xj + 1];
        i++;
        p_index_xj++;
    }
    return total;
}

int main(void) {
    int d[5] = {1, 2, 3, 4, 5};
    printf("%d\n", lookaround(d, 5));
    return 0;
}
