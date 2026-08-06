#include <stdio.h>

/* `p` is initialized from `q`, but `p` itself cannot be transformed (its
 * address is taken), so nothing rewrites `p`'s declaration wholesale.
 * `q`'s reference inside it therefore has to be materialized rather than
 * suppressed — leaving it bare would name a variable collapse deletes.
 * Only half the pair transforms, which is fine; what must not happen is
 * `q` being collapsed with its name left behind. */
static int guarded(const int *buf, int n) {
    int q_index_xj = 0;
    const int *p = (buf + q_index_xj);
    const int **alias = &p;
    int s = 0;
    for (int i = 0; i < n; i++) {
        s += buf[q_index_xj];
        q_index_xj++;
    }
    return s + **alias;
}

int main(void) {
    int d[4] = {1, 2, 4, 8};
    printf("%d\n", guarded(d, 4));
    return 0;
}
