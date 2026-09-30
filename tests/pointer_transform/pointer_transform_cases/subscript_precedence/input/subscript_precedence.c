#include <stdio.h>

/* A subscript joins the index after a `+`, so one that binds more loosely
 * than `+` needs parentheses to stay one operand. */

static int pick(int *tbl, int i, int c) {
    int *p = tbl;
    p += 2;
    return p[i & 3] * 100 + p[i >> 1] * 10 + p[c ? 1 : 2];
}

/* The subscript of `&arr[i]` becomes a term of the index the same way. */
static int pick_element(int i, int c) {
    static int arr[16] = {0, 1, 2,  3,  4,  5,  6,  7,
                          8, 9, 10, 11, 12, 13, 14, 15};
    int *q;
    int *r;
    q = &arr[i & 3] + 1;
    q++;
    r = &arr[c ? 1 : 2] + 4;
    r++;
    int *s = &arr[i | 8];
    s++;
    return *q * 10000 + *r * 100 + *s + q[i << 1] + s[i == 5];
}

int main(void) {
    int t[] = {0, 1, 2, 3, 4, 5, 6, 7};
    printf("%d %d\n", pick(t, 5, 0), pick_element(5, 0));
    return 0;
}
