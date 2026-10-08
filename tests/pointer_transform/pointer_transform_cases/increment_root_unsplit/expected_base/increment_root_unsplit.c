#include <stdio.h>

/* q = p++ pairs q's index with p's, which needs p to be rewritten. */

/* The step is p's only motion. It still counts as one, so p is rewritten
   and the pair stands. */
static int step_is_only_motion(char *s) {
    int p_index_xj = 0;
    int q_index_xj = p_index_xj++;
    q_index_xj++;
    return s[q_index_xj] + s[p_index_xj];
}

/* In the rest p is declined because its address is taken, so there is no
   p_index_xj for the step to move. The right-hand side is kept whole and
   q's index starts at 0. */

static int address_taken(char *buf) {
    char *p = buf;
    char **pp = &p;
    char *q;
    int q_index_xj = 0;
    q = p++, q_index_xj = 0;
    q_index_xj++;
    q[q_index_xj] = 'x';
    return (int)(*pp - buf);
}

/* The assignment's value is read, and the step is a pre-decrement. */
static int value_read(char *buf, int n) {
    char *p = buf + n;
    char **pp = &p;
    char *q;
    int q_index_xj = 0;
    int seen = 0;
    while ((q = --p, q_index_xj = 0, (q + q_index_xj)) > buf) {
        q_index_xj--;
        seen += q[q_index_xj];
    }
    return seen + (int)(*pp - buf);
}

/* An offset that reads through a rewritten pointer stays in the right-hand
   side, and is still rewritten there. */
static int offset_through_other(char *buf) {
    char *p = buf;
    char **pp = &p;
    int r_index_xj = 0;
    r_index_xj++;
    char *q = p++ + (r_index_xj);
    int q_index_xj = 0;
    q_index_xj++;
    return q[q_index_xj] + (int)(*pp - buf);
}

/* File-scope pointers pair with each other the same way. */
static char *g_cur;
static char *g_mark;
static int g_mark_index_xj = 0;

static int file_scope(char *s) {
    char **cur = &g_cur;
    g_cur = s;
    g_mark = g_cur++, g_mark_index_xj = 0;
    g_mark_index_xj++;
    return g_mark[g_mark_index_xj] + (int)(*cur - s);
}

int main(void) {
    char a[] = "abcde";
    char b[] = "abcde";
    char c[] = "abcde";
    char d[] = "abcde";
    char e[] = "abcde";
    printf("%d\n", step_is_only_motion(a));
    printf("%d %s\n", address_taken(b), b);
    printf("%d\n", value_read(c, 5));
    printf("%d\n", offset_through_other(d));
    printf("%d\n", file_scope(e));
    return 0;
}
