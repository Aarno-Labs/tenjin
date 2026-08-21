#include <stdio.h>
#include <string.h>

/* The allowlist's second entry. zmy carried a strstr wrapper body that
 * nothing could reach, because the name itself was never allowlisted. */
static int count_needles(const char *hay, const char *needle) {
    const char *p = hay;
    int n = 0;

    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p++;
    }
    return n;
}

int main(void) {
    printf("%d\n", count_needles("abababa", "aba"));
    printf("%d\n", count_needles("xyz", "aba"));
    return 0;
}
