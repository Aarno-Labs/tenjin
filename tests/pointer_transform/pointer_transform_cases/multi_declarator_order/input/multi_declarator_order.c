#include <stdio.h>

/* An index is declared after its pointer's whole statement, or ahead of
 * the loop for a for-init, and that is where its initializer runs. What
 * would be carried across a sibling declarator that could tell stays in
 * the pointer's own initializer instead. */

/* A step carried past a sibling that reads the stepped pointer. */
static int pair(char *s) {
    char *p = s;
    p++;
    char *a = p++, c = *p;
    a++;
    return (*a == c) + (int)(p - s);
}

/* A sibling's step, which the index would read too late. */
static int sibling_steps(char *s) {
    char *p = s;
    p++;
    char *a = p, c = *p++;
    a++;
    return (*a) * 1000 + c + (int)(p - s);
}

/* Ahead of the loop, a step is carried across the siblings before it. */
static int hoisted(char *s) {
    char *p = s;
    p++;
    int n = 0;
    for (char c = *p, *a = p++; *a; a++)
        n = n * 10 + (*a - c);
    return n * 10 + (int)(p - s);
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
