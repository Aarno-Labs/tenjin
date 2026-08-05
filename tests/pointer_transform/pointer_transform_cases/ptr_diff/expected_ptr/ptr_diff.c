#include <stdio.h>

/* Two tracked, moving pointers over one base: `w < r` compares them and
 * `w - buf` subtracts the base. Both should reduce to index arithmetic
 * rather than being rebuilt as pointer expressions. */
static int compact(int *buf, int n) {
    int r_index_xj = 0;
    int w_index_xj = 0;
    int moved = 0;
    for (int i = 0; i < n; i++) {
        if (buf[r_index_xj]) {
            buf[w_index_xj] = buf[r_index_xj];
            if (r_index_xj > w_index_xj)
                moved++;
            w_index_xj++;
        }
        r_index_xj++;
    }
    return (int)(w_index_xj) * 10 + moved;
}

int main(void) {
    int d[6] = {0, 3, 0, 5, 7, 0};
    printf("%d\n", compact(d, 6));
    return 0;
}
