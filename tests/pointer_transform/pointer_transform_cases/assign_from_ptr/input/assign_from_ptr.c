#include <stdio.h>

/* `p` is initialized from another tracked, moving pointer plus an
 * offset, so its index must be inherited as `q_index + 1` rather than
 * captured from a base text that is itself moving. */
static int windows(const int *buf, int n) {
    const int *q = buf;
    int s = 0;
    for (int i = 0; i + 1 < n; i++) {
        const int *p = q + 1;
        s += *p - *q;
        q++;
    }
    return s;
}

int main(void) {
    int d[4] = {1, 4, 9, 16};
    printf("%d\n", windows(d, 4));
    return 0;
}
