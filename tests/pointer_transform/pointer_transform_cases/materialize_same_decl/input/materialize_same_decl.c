#include <stdio.h>

/* `p` and `q` share a DeclStmt, so p's index variable is declared after
 * the whole statement. Materializing p inside q's initializer would name
 * that index before it exists, so this pair must be left alone. */
static int f(int *a, int *b, int n) {
    int *p = a;
    if (n < 0)
        p = b;
    p++;
    int *r = p, *q = r + 1;
    return *r + *q;
}

int main(void) {
    int a[4] = {1, 2, 4, 8};
    int b[4] = {10, 20, 40, 80};
    printf("%d %d\n", f(a, b, 1), f(a, b, -1));
    return 0;
}
