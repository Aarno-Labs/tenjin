#include <stdio.h>

/* A subscript joins the index after a `+`, so one that binds more loosely
 * than `+` needs parentheses to stay one operand. */

static int pick(int *tbl, int i, int c) {
    int *p = tbl;
    int p_index_xj = 0;
    p_index_xj += 2;
    return p[p_index_xj + (i & 3)] * 100 + p[p_index_xj + (i >> 1)] * 10 + p[p_index_xj + (c ? 1 : 2)];
}

/* The subscript of `&arr[i]` becomes a term of the index the same way. */
static int pick_element(int i, int c) {
    static int arr[16] = {0, 1, 2,  3,  4,  5,  6,  7,
                          8, 9, 10, 11, 12, 13, 14, 15};
    int *q;
    int q_index_xj = 0;
    int *r;
    int r_index_xj = 0;
    (q = arr, q_index_xj = (i & 3) + 1);
    q_index_xj++;
    (r = arr, r_index_xj = (c ? 1 : 2) + 4);
    r_index_xj++;
    int *s = arr;
    int s_index_xj = (i | 8);
    s_index_xj++;
    return q[q_index_xj] * 10000 + r[r_index_xj] * 100 + s[s_index_xj] + q[q_index_xj + (i << 1)] + s[s_index_xj + (i == 5)];
}

int main(void) {
    int t[] = {0, 1, 2, 3, 4, 5, 6, 7};
    printf("%d %d\n", pick(t, 5, 0), pick_element(5, 0));
    return 0;
}
