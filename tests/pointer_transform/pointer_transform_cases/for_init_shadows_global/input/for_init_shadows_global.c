#include <stdio.h>

/* The index of a for-init pointer is declared ahead of its loop, where it
 * outlives the pointer. Named like the index of a file-scope pointer, it
 * would go on shadowing that one after the loop. */

static char *cur;

static int f(char *s) {
    int n = 0;
    for (char *cur = s; *cur; cur++)
        n++;
    cur++;
    return n + *cur;
}

int main(void) {
    static char g[] = "AB";
    char s[] = "xyz";
    cur = g;
    printf("%d\n", f(s));
    return 0;
}
