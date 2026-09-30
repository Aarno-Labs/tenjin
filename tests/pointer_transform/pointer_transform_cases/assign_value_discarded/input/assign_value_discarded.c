#include <stdio.h>

/* Pointer assignments whose value is discarded. */

/* for-init and for-increment, each the arm of a comma. */
static int count_odd(const char *s) {
    const char *p;
    int n;
    for (p = s + 1, n = 0; *p; n++, p = p + 2)
        ;
    return n;
}

/* Parenthesized statement. */
static int third(const char *s) {
    const char *p = s;
    p++;
    (p = s + 2);
    return *p;
}

int main(void) {
    char text[] = "abcdefg";
    printf("%d %c\n", count_odd(text), third(text));
    return 0;
}
