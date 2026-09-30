#include <stdio.h>

/* Compound pointer assignments whose value is used. */

static int take(const char *q) { return *q; }

/* Comparison, as a loop condition. */
static int count_pairs(const char *s, const char *end) {
    int p_index_xj = 0;
    int n = 0;
    while (((s + (p_index_xj += 2))) <= end)
        n++;
    return n;
}

/* Dereference. */
static char third(char *s) {
    int p_index_xj = 0;
    p_index_xj++;
    return *((s + (p_index_xj += 1)));
}

/* Call argument. */
static int back_two(const char *s) {
    int p_index_xj = 0;
    p_index_xj += 4;
    return take((s + (p_index_xj -= 2)));
}

/* The operand reads through another rewritten pointer. */
static int skip_by(const char *s, const char *widths) {
    int p_index_xj = 0;
    int w_index_xj = 0;
    int n = 0;
    while (widths[w_index_xj])
        n += *((s + (p_index_xj += widths[w_index_xj++] - '0')));
    return n;
}

/* Value discarded. */
static int walk(const char *s) {
    int p_index_xj = 0;
    int q_index_xj = 0;
    int n = 0;
    p_index_xj += 1;
    for (; s[p_index_xj]; p_index_xj += 2, (q_index_xj += 1))
        n += s[q_index_xj];
    return n + (int)(p_index_xj);
}

int main(void) {
    char text[] = "abcdefg";
    printf("%d %c %d %d %d\n", count_pairs(text, text + 7), third(text),
           back_two(text), skip_by(text, "123"), walk(text));
    return 0;
}
