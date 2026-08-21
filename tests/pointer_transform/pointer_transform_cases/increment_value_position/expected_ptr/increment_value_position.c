#include <stdio.h>

/* Pointer increments whose value is used. */

static int take(const char *q) { return *q; }

/* Comparison. */
static int count_below(char *p, char *end) {
    int p_index_xj = 0;
    int n = 0;
    while ((p + p_index_xj++) < end)
        n += 1;
    return n;
}

/* Call argument. */
static int sum_via_call(char *p) {
    int p_index_xj = 0;
    int n = 0;
    while (p[p_index_xj])
        n += take((p + p_index_xj++));
    return n;
}

/* Cast. */
static int cast_escapes(char *p) {
    int p_index_xj = 0;
    void *v = (void *)(p + p_index_xj++);
    while (p[p_index_xj])
        p_index_xj++;
    return v != 0;
}

/* Pointer difference. */
static long distance(char *p, char *base) {
    int p_index_xj = 0;
    long d = (p + p_index_xj++) - base;
    while (p[p_index_xj])
        p_index_xj++;
    return d + 1;
}

/* Value discarded. */
static int walk(char *p) {
    int p_index_xj = 0;
    int n = 0;
    p_index_xj++;
    while (p[p_index_xj]) {
        n += 1;
        p_index_xj++;
    }
    return n;
}

int main(void) {
    char text[] = "abcd";
    printf("%d %d %d %ld %d\n", count_below(text, text + 4), sum_via_call(text),
           cast_escapes(text), distance(text + 1, text), walk(text));
    return 0;
}
