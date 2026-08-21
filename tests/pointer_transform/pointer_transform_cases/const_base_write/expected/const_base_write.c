#include <stdio.h>

/* Writing through a pointer whose base is const-qualified. The old
 * rewrite would have emitted the write against the const base directly,
 * which does not compile, so it declined the pointer; the retained
 * pointer keeps the cast the source already performed. */
static void patch(const char *tmpl, int n) {
    char *w = (char *)tmpl;
    int w_index_xj = 0;
    for (int i = 0; i < n; i++)
        w[w_index_xj++] = 'x';
}

int main(void) {
    char buf[6] = "abcde";
    patch(buf, 3);
    printf("%s\n", buf);
    return 0;
}
