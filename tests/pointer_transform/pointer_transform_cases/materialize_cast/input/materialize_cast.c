#include <stdio.h>

/* The materialized reference sits under a cast, so it must be
 * parenthesized: `(char *)p` becoming `(char *)p + p_index_xj` would
 * scale the offset by bytes instead of ints. The correct form is
 * `(char *)(p + p_index_xj)`. */
static int f(int *a, int *b, int n) {
    int *p = a;
    if (n < 0)
        p = b;
    p++;
    char *c = (char *)p;
    c++;
    return *p + (int)c[0];
}

int main(void) {
    int a[4] = {1, 2, 4, 8};
    int b[4] = {10, 20, 40, 80};
    printf("%d %d\n", f(a, b, 1), f(a, b, -1));
    return 0;
}
