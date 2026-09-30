#include <stdio.h>

/* Two pointers declared in one statement are not reconstructed. */

static int pairwise(int *a, int n) {
    int *p = a, *q = a + 1;
    int s = 0;
    for (int i = 0; i + 1 < n; i++)
        s += *p++ * 2 + *q++;
    return s;
}

int main(void) {
    int v[5] = {1, 2, 3, 4, 5};
    printf("%d\n", pairwise(v, 5));
    return 0;
}
