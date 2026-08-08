#include <stdio.h>

/* `p` is assigned from two different bases depending on a condition. The
 * single-base scheme has no one text to substitute, so it rejects the
 * pointer outright; the frozen-handle scheme keeps `p` and resets its
 * index at the reseat. */
static int scan(const int *primary, const int *fallback, int n, int use_fb) {
    const int *p = primary;
    int p_index_xj = 0;
    if (use_fb)
        (p = fallback, p_index_xj = 0);
    int total = 0;
    for (int i = 0; i < n; i++)
        total += p[p_index_xj++];
    return total;
}

int main(void) {
    int a[4] = {1, 2, 3, 4};
    int b[4] = {10, 20, 30, 40};
    printf("%d %d\n", scan(a, b, 4, 0), scan(a, b, 4, 1));
    return 0;
}
