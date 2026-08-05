#include <stdio.h>

/* The materialized reference sits under a cast, so it must be
 * parenthesized: `(char *)p` becoming `(char *)p + p_index_xj` would
 * scale the offset by bytes instead of ints. The correct form is
 * `(char *)(p + p_index_xj)`. */
static int f(int *a, int *b, int n) {
    int *p = a;
    int p_index_xj = 0;
    if (n < 0)
        (p = b, p_index_xj = 0);
    p_index_xj++;
    char *c = (char *)(p + p_index_xj);
    int c_index_xj = 0;
    c_index_xj++;
    return p[p_index_xj] + (int)c[c_index_xj];
}

int main(void) {
    int a[4] = {1, 2, 4, 8};
    int b[4] = {10, 20, 40, 80};
    printf("%d %d\n", f(a, b, 1), f(a, b, -1));
    return 0;
}
