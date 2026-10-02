#include <stdio.h>

/* q is initialized from an increment or decrement of p. */

static int post(int *a, int n) {
    int *p = a;
    int s = 0;
    for (int i = 0; i < n / 2; i++) {
        s += *p;
        p++;
    }
    int *q = p++;
    while (q - a < n) {
        s += *q * 2;
        q++;
    }
    return s;
}

static int pre(int *a, int n) {
    int *p = a;
    int s = 0;
    for (int i = 0; i < n / 2; i++) {
        s += *p;
        p++;
    }
    int *q = ++p;
    while (q - a < n) {
        s += *q * 3;
        q++;
    }
    return s;
}

static int post_dec(int *a, int n) {
    int *p = a + n - 1;
    int s = 0;
    for (int i = 0; i < n / 2; i++) {
        s += *p;
        p--;
    }
    int *q = p--;
    while (q >= a) {
        s += *q * 5;
        q--;
    }
    return s;
}

static int stepped_offset(int *a, int n) {
    int *p = a;
    int s = 0;
    for (int i = 0; i < n / 2; i++) {
        s += *p;
        p++;
    }
    int *q = p++ + 1;
    while (q - a < n) {
        s += *q * 7;
        q++;
    }
    return s;
}

int main(void) {
    int v[6] = {1, 2, 4, 8, 16, 32};
    printf("%d %d %d %d\n", post(v, 6), pre(v, 6), post_dec(v, 6),
           stepped_offset(v, 6));
    return 0;
}
