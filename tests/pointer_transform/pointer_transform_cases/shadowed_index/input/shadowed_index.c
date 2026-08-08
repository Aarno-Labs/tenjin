#include <stdio.h>

/* Two pointers named `p` in different scopes, both frozen. The inner one
 * is declared in a for-init, so its index is placed before the `for` —
 * escaping the pointer's own scope into the block that already holds the
 * outer `p`'s index. Without per-pointer index names the two collide;
 * with them, the use of the outer `p` after the loop still reads the
 * outer index. */
static int f(int *a, int *b, int n) {
    int *p = a;
    if (n < 0)
        p = b;
    p++;
    int s = *p;

    for (int *p = b; n > 0; n--) {
        if (n == 1)
            p = a;
        s += *p;
        p++;
    }

    s += *p;
    return s;
}

int main(void) {
    int a[6] = {1, 2, 4, 8, 16, 32};
    int b[6] = {10, 20, 40, 80, 160, 320};
    printf("%d %d\n", f(a, b, 3), f(a, b, -1));
    return 0;
}
