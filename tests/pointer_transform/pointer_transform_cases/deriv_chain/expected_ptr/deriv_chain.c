#include <stdio.h>

/* Each pointer is initialized from the previous one plus an offset. */
static int chain(int *a, int n) {
    int *p = a;
    int p_index_xj = 0;
    int *q = p;
    int q_index_xj = p_index_xj + 1;
    int *r = q;
    int r_index_xj = q_index_xj + 1;
    int s = 0;
    while ((r + r_index_xj) - a < n) {
        s += p[p_index_xj] + q[q_index_xj] * 2 + r[r_index_xj] * 4;
        p_index_xj++;
        q_index_xj++;
        r_index_xj++;
    }
    return s;
}

int main(void) {
    int v[6] = {1, 2, 3, 4, 5, 6};
    printf("%d\n", chain(v, 6));
    return 0;
}
