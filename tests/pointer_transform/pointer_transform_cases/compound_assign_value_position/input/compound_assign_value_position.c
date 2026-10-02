#include <stdio.h>

/* Compound pointer assignments whose value is used. */

static int take(const char *q) { return *q; }

/* Comparison, as a loop condition. */
static int count_pairs(const char *s, const char *end) {
    const char *p = s;
    int n = 0;
    while ((p += 2) <= end)
        n++;
    return n;
}

/* Dereference. */
static char third(char *s) {
    char *p = s;
    p++;
    return *(p += 1);
}

/* Call argument. */
static int back_two(const char *s) {
    const char *p = s;
    p += 4;
    return take(p -= 2);
}

/* The operand reads through another rewritten pointer. */
static int skip_by(const char *s, const char *widths) {
    const char *p = s;
    const char *w = widths;
    int n = 0;
    while (*w)
        n += *(p += *w++ - '0');
    return n;
}

/* Value discarded. */
static int walk(const char *s) {
    const char *p = s;
    const char *q = s;
    int n = 0;
    p += 1;
    for (; *p; p += 2, (q += 1))
        n += *q;
    return n + (int)(p - s);
}

int main(void) {
    char text[] = "abcdefg";
    printf("%d %c %d %d %d\n", count_pairs(text, text + 7), third(text),
           back_two(text), skip_by(text, "123"), walk(text));
    return 0;
}
