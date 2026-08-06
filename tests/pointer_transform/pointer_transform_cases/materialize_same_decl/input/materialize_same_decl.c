#include <stdio.h>

/* `r` and `q` share a DeclStmt, and `q`'s initializer names `r`. Since
 * `r` is frozen, that reference has to be materialized as
 * `(r + r_index_xj)` — inside the very statement `r` is declared in. That
 * only works because a frozen pointer's index is declared *before* the
 * statement rather than after it. */
static int f(int *a, int *b, int n) {
    int *r = a, *q = r + 1;
    if (n < 0)
        r = b;
    int s = 0;
    for (int i = 0; i + 1 < n; i++) {
        s += *r + *q;
        r++;
        q++;
    }
    return s;
}

int main(void) {
    int a[5] = {1, 2, 4, 8, 16};
    int b[5] = {10, 20, 40, 80, 160};
    printf("%d %d\n", f(a, b, 4), f(a, b, -4));
    return 0;
}
