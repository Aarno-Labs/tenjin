#include <stdio.h>

/* `p` and `q` share a DeclStmt, so p's index variable is declared after
 * the whole statement. Materializing p inside q's initializer would name
 * that index before it exists, so this pair must be left alone. */
static int f(int *a, int *b, int n) {
    int *p = a;
    int p_index_xj = 0;
    if (n < 0)
        (p = b, p_index_xj = 0);
    p_index_xj++;
    int *r = (p + p_index_xj), *q = r + 1;
    int q_index_xj = 1;
    return *r + r[q_index_xj];
}

int main(void) {
    int a[4] = {1, 2, 4, 8};
    int b[4] = {10, 20, 40, 80};
    printf("%d %d\n", f(a, b, 1), f(a, b, -1));
    return 0;
}
