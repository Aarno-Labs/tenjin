#include <stdio.h>
#include <string.h>

/* The value of a pointer assignment is used in a condition. */
static int strchr_index_xj(const char *base, int start, int c) {
    const char *result = strchr(base + start, c);
    if (!result) return -1;
    return (int)(result - base);
}

static int count_fields(char *s) {
    char *p = s;
    int p_index_xj = 0;
    int n = 0;
    while (((p_index_xj = strchr_index_xj(p, p_index_xj, ','), (p_index_xj < 0 ? (void *)0 : p + p_index_xj))) != NULL) {
        n++;
        p_index_xj++;
    }
    return n;
}

int main(void) {
    char text[] = "a,bb,ccc,";
    printf("%d\n", count_fields(text));
    return 0;
}
