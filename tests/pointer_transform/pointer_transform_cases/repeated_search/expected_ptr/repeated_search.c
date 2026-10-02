#include <stdio.h>
#include <string.h>

/* Two strchr results are assigned to the same pointer. */
static int strchr_index_xj(const char *base, int start, int c) {
    const char *result = strchr(base + start, c);
    if (!result) return -1;
    return (int)(result - base);
}

static int two_searches(const char *s) {
    const char *p = s;
    int p_index_xj = 0;
    int n = 0;

    p_index_xj = strchr_index_xj(p, p_index_xj, ',');
    if (!(p && p_index_xj >= 0))
        return 0;
    p_index_xj++;

    p_index_xj = strchr_index_xj(p, p_index_xj, ';');
    if (!(p && p_index_xj >= 0))
        return 1;

    n = 2;
    while (p[p_index_xj]) {
        n++;
        p_index_xj++;
    }
    return n;
}

int main(void) {
    printf("%d\n", two_searches("a,b;cd"));
    printf("%d\n", two_searches("a,b"));
    printf("%d\n", two_searches("ab"));
    return 0;
}
