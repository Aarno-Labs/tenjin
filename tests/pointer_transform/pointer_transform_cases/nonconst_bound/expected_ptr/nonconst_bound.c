#include <stdio.h>

/* The bound expression `buf + n - 1` is not a recognized slice shape, so
 * the function keeps its (ptr, len) signature; the iterating pointer is
 * still rewritten to an index. */
static int adjacent_pairs(const int *buf, int n) {
    const int *p = buf;
    int p_index_xj = 0;
    int count = 0;
    while ((p + p_index_xj) < buf + n - 1) {
        if (p[p_index_xj] < p[p_index_xj + 1])
            count++;
        p_index_xj++;
    }
    return count;
}

int main(void) {
    int d[6] = {1, 3, 2, 5, 4, 6};
    printf("%d\n", adjacent_pairs(d, 6));
    return 0;
}
