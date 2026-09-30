#include <stdio.h>

/* Pointer assignments whose value is discarded. */

/* for-init and for-increment, each the arm of a comma. */
static int count_odd(const char *s) {
    int p_index_xj = 0;
    int n;
    for ((p_index_xj = 1), n = 0; s[p_index_xj]; n++, (p_index_xj = p_index_xj + 2))
        ;
    return n;
}

/* Parenthesized statement. */
static int third(const char *s) {
    int p_index_xj = 0;
    p_index_xj++;
    (p_index_xj = 2);
    return s[p_index_xj];
}

int main(void) {
    char text[] = "abcdefg";
    printf("%d %c\n", count_odd(text), third(text));
    return 0;
}
