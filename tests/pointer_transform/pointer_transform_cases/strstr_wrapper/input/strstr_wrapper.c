#include <stdio.h>
#include <string.h>

/* The value of a strstr assignment is used in a condition. */
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
