#include <stdio.h>

/* Two tracked, moving pointers over one base: `w < r` compares them and
 * `w - buf` subtracts the base. Both should reduce to index arithmetic
 * rather than being rebuilt as pointer expressions. */
static int compact(int *buf, int n) {
    int *r = buf;
    int *w = buf;
    int moved = 0;
    for (int i = 0; i < n; i++) {
        if (*r) {
            *w = *r;
            if (w < r)
                moved++;
            w++;
        }
        r++;
    }
    return (int)(w - buf) * 10 + moved;
}

int main(void) {
    int d[6] = {0, 3, 0, 5, 7, 0};
    printf("%d\n", compact(d, 6));
    return 0;
}
