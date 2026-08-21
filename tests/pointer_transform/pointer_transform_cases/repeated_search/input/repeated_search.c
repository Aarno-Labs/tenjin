#include <stdio.h>
#include <string.h>

/* Two searches over one pointer. The second starts from the index the
 * first left behind, and the wrapper is emitted once: a pointer only
 * counts as transformed after its whole rewrite lands, so the dedupe has
 * to see the body this same pointer already queued. */
static int two_searches(const char *s) {
    const char *p = s;
    int n = 0;

    p = strchr(p, ',');
    if (!p)
        return 0;
    p++;

    p = strchr(p, ';');
    if (!p)
        return 1;

    n = 2;
    while (*p) {
        n++;
        p++;
    }
    return n;
}

int main(void) {
    printf("%d\n", two_searches("a,b;cd"));
    printf("%d\n", two_searches("a,b"));
    printf("%d\n", two_searches("ab"));
    return 0;
}
