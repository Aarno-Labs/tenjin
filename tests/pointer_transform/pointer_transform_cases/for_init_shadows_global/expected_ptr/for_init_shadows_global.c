#include <stdio.h>

/* The index of a for-init pointer is declared ahead of its loop, where it
 * outlives the pointer. Named like the index of a file-scope pointer, it
 * would go on shadowing that one after the loop. */

static char *cur;
static int cur_index_xj = 0;

static int f(char *s) {
    int n = 0;
    int cur_index_xj_1 = 0;
    for (char *cur = s; cur[cur_index_xj_1]; cur_index_xj_1++)
        n++;
    cur_index_xj++;
    return n + cur[cur_index_xj];
}

int main(void) {
    static char g[] = "AB";
    char s[] = "xyz";
    cur = g, cur_index_xj = 0;
    printf("%d\n", f(s));
    return 0;
}
