#include <stdio.h>

/* A handle-mode pointer declared in a for-init. The index declaration
 * cannot go after the DeclStmt — that slot is the loop condition — so it
 * must be placed before the whole `for` statement. */
static int f(int *a, int *b, int n) {
    int s = 0;
    for (int *p = a; n > 0; n--) {
        if (n == 2)
            p = b;
        s += *p;
        p++;
    }
    return s;
}

int main(void) {
    int a[4] = {1, 2, 4, 8};
    int b[4] = {10, 20, 40, 80};
    printf("%d\n", f(a, b, 3));
    return 0;
}
