#include <stdio.h>

/* Pointer increments whose value is used. */

static int take(const char *q) { return *q; }

/* Comparison. */
static int count_below(char *p, char *end) {
    int n = 0;
    while (p++ < end)
        n += 1;
    return n;
}

/* Call argument. */
static int sum_via_call(char *p) {
    int n = 0;
    while (*p)
        n += take(p++);
    return n;
}

/* Cast. */
static int cast_escapes(char *p) {
    void *v = (void *)p++;
    while (*p)
        p++;
    return v != 0;
}

/* Pointer difference. */
static long distance(char *p, char *base) {
    long d = p++ - base;
    while (*p)
        p++;
    return d + 1;
}

/* Value discarded. */
static int walk(char *p) {
    int n = 0;
    p++;
    while (*p) {
        n += 1;
        p++;
    }
    return n;
}

int main(void) {
    char text[] = "abcd";
    printf("%d %d %d %ld %d\n", count_below(text, text + 4), sum_via_call(text),
           cast_escapes(text), distance(text + 1, text), walk(text));
    return 0;
}
