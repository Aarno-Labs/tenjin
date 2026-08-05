#include <stdio.h>

/* `p` is initialized from `q`, but `p` itself cannot be transformed (its
 * address is taken). `q` must therefore stay a pointer too: collapsing it
 * would delete its declaration while `p = q` still names it. Guards the
 * inheritance path against transforming only half of a pair. */
static int guarded(const int *buf, int n) {
    const int *q = buf;
    const int *p = q;
    const int **alias = &p;
    int s = 0;
    for (int i = 0; i < n; i++) {
        s += *q;
        q++;
    }
    return s + **alias;
}

int main(void) {
    int d[4] = {1, 2, 4, 8};
    printf("%d\n", guarded(d, 4));
    return 0;
}
