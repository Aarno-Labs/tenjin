#include <stdio.h>
#include <string.h>

/* Both halves of the (region, offset) pair can say NULL, and a pointer can
 * reach either one:
 *
 *   - pick_region is opaque to the pass, so `p = pick_region(...)` is an
 *     ordinary reseat: the region may be null while the offset stays 0.
 *   - `p = strchr(...)` leaves the region alone and drives the offset to
 *     -1 when there is no match.
 *
 * A test of only the offset would call the first case non-null, and a test
 * of only the region would call the second case non-null, so `if (!p)` has
 * to consult both. */

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
