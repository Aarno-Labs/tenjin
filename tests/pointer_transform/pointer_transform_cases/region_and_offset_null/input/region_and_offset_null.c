#include <stdio.h>
#include <string.h>

/* p may be null from an opaque call or from a failed strchr. */

static char storage[32];

static char *pick_region(int ok) {
    return ok ? storage : (char *)0;
}

static int tail_after(int ok, int c) {
    char *p = pick_region(ok);
    int n = 0;

    if (!p)
        return -1;

    p = strchr(p, c);
    if (!p)
        return -2;

    p++;
    while (*p) {
        n++;
        p++;
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
