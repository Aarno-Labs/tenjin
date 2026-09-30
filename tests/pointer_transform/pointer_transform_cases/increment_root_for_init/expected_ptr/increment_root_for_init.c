#include <stdio.h>

/* q = p++ with both pointers declared in the same for-init. */

static int paired(int *a, int n) {
    int s = 0;
    int p_index_xj = 0;
    int q_index_xj = p_index_xj++;
    for (int *p = a, *q = p; (p + p_index_xj) - a < n; p_index_xj++, q_index_xj++)
        s += p[p_index_xj] * 2 + q[q_index_xj];
    return s;
}

/* The same with a constant offset. */
static int paired_second(int *a, int n) {
    int s = 0;
    int p_index_xj = 0;
    int q_index_xj = p_index_xj + 1;
    for (int *p = a, *q = p; (p + p_index_xj) - a < n; p_index_xj++, q_index_xj++)
        s += p[p_index_xj] + q[q_index_xj] * 3;
    return s;
}

int main(void) {
    int v[8] = {1, 2, 4, 8, 16, 32, 64, 128};
    printf("%d %d\n", paired(v, 4), paired_second(v, 4));
    return 0;
}
