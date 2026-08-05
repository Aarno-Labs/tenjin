#include <stdio.h>

/* `p` is initialized from another tracked, moving pointer plus an
 * offset, so its index must be inherited as `q_index + 1` rather than
 * captured from a base text that is itself moving. */
static int windows(const int *buf, int n) {
    int q_index_xj = 0;
    int s = 0;
    for (int i = 0; i + 1 < n; i++) {
        int p_index_xj = q_index_xj + (1);
        s += buf[p_index_xj] - buf[q_index_xj];
        q_index_xj++;
    }
    return s;
}

int main(void) {
    int d[4] = {1, 4, 9, 16};
    printf("%d\n", windows(d, 4));
    return 0;
}
