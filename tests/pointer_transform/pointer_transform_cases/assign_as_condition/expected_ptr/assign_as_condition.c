#include <stdio.h>
#include <string.h>

/* A pointer assignment that is itself the truth test. */

/* Loop condition, in the conventional extra parentheses. */
static int strchr_index_xj(const char *base, int start, int c) {
    const char *result = strchr(base + start, c);
    if (!result) return -1;
    return (int)(result - base);
}

static int count_fields(const char *s) {
    const char *p = s;
    int p_index_xj = 0;
    int n = 0;
    while (((p_index_xj = strchr_index_xj(p, p_index_xj, ',')) >= 0)) {
        n++;
        p_index_xj++;
    }
    return n;
}

/* Operand of &&. */
static int count_up_to(const char *s, int limit) {
    const char *p = s;
    int p_index_xj = 0;
    int n = 0;
    while (n < limit && ((p_index_xj = strchr_index_xj(p, p_index_xj, ',')) >= 0)) {
        n++;
        p_index_xj++;
    }
    return n;
}

/* if condition. */
static int strstr_index_xj(const char *base, int start, const char *needle) {
    const char *result = strstr(base + start, needle);
    if (!result) return -1;
    return (int)(result - base);
}

static int offset_after_first(const char *s, const char *needle) {
    const char *p = s;
    int p_index_xj = 0;
    p_index_xj++;
    if (((p_index_xj = strstr_index_xj(p, p_index_xj, needle)) >= 0))
        return (int)((p_index_xj < 0 ? (void *)0 : p + p_index_xj) - s);
    return -1;
}

int main(void) {
    printf("%d %d %d %d\n", count_fields("a,bb,ccc,"), count_up_to("a,bb,ccc,", 2),
           offset_after_first("hello world", "wor"),
           offset_after_first("hello world", "xyz"));
    return 0;
}
