#include <stdio.h>

/* An index is declared after its pointer's whole statement, or ahead of
 * the loop for a for-init, and that is where its initializer runs. What
 * would be carried across a sibling declarator that could tell stays in
 * the pointer's own initializer instead. */

/* A step carried past a sibling that reads the stepped pointer. */
static int pair(char *s) {
    char *p = s;
    int p_index_xj = 0;
    p_index_xj++;
    char *a = (p + p_index_xj++), c = p[p_index_xj];
    int a_index_xj = 0;
    a_index_xj++;
    return (a[a_index_xj] == c) + (int)((p + p_index_xj) - s);
}

/* A sibling's step, which the index would read too late. */
static int sibling_steps(char *s) {
    char *p = s;
    int p_index_xj = 0;
    p_index_xj++;
    char *a = (p + p_index_xj), c = p[p_index_xj++];
    int a_index_xj = 0;
    a_index_xj++;
    return (a[a_index_xj]) * 1000 + c + (int)((p + p_index_xj) - s);
}

/* Ahead of the loop, a step is carried across the siblings before it. */
static int hoisted(char *s) {
    char *p = s;
    int p_index_xj = 0;
    p_index_xj++;
    int n = 0;
    int a_index_xj = 0;
    for (char c = p[p_index_xj], *a = (p + p_index_xj++); a[a_index_xj]; a_index_xj++)
        n = n * 10 + (a[a_index_xj] - c);
    return n * 10 + (int)((p + p_index_xj) - s);
}

/* A sibling that reads through the pointer needs an index that is not
 * declared yet, so the pointer is left alone. */
static int sibling_reads(int *a) {
    int *p = a + 1, v = *p;
    p++;
    return v * 100 + *p;
}

int main(void) {
    char s[] = "wxyz";
    int t[] = {1, 2, 3, 4};
    printf("%d %d %d %d\n", pair(s), sibling_steps(s), hoisted(s),
           sibling_reads(t));
    return 0;
}
