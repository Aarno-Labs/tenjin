#include <stdio.h>

/* q is declared as a plain copy of p. */
static int tail_sum(int *a, int n) {
    int *p = a;
    int s = 0;
    for (int i = 0; i < n / 2; i++) {
        s += *p;
        p++;
    }
    int *q = p;
    while (q - a < n) {
        s += *q * 2;
        q++;
    }
    return s;
}

int main(void) {
    int v[6] = {1, 2, 3, 4, 5, 6};
    printf("%d\n", tail_sum(v, 6));
    return 0;
}
