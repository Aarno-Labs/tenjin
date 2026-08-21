#include <stdio.h>
#include <string.h>

/* p may be null from an opaque call or from a failed strchr. */

static char storage[32];

static char *pick_region(int ok) {
    return ok ? storage : (char *)0;
}

static int strchr_index_xj(const char *base, int start, int c) {
    const char *result = strchr(base + start, c);
    if (!result) return -1;
    return (int)(result - base);
}

static int tail_after(int ok, int c) {
    char *p = pick_region(ok);
    int p_index_xj = 0;
    int n = 0;

    if (!(p && p_index_xj >= 0))
        return -1;

    p_index_xj = strchr_index_xj(p, p_index_xj, c);
    if (!(p && p_index_xj >= 0))
        return -2;

    p_index_xj++;
    while (p[p_index_xj]) {
        n++;
        p_index_xj++;
    }
    return n;
}

int main(void) {
    strcpy(storage, "ab,cdef");
    printf("%d\n", tail_after(1, ','));   /* found: 4 */
    printf("%d\n", tail_after(1, '@'));   /* no match: -2 */
    printf("%d\n", tail_after(0, ','));   /* null region: -1 */
    return 0;
}
