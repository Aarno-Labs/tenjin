#include <stdio.h>
#include <string.h>

/* Two strchr results are assigned to the same pointer. */
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
