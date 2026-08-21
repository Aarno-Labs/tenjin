#include <stdio.h>
#include <string.h>

/* Uses of the pointer's value that are not element accesses: returned,
 * differenced against its origin, handed to a library function. Each is
 * rebuilt in place as `(p + p_index_xj)`, which is the fallback that
 * lets the rewrite be total instead of declining what it cannot index. */
static char *skip_spaces(char *s) {
    int s_index_xj = 0;
    while (s[s_index_xj] == ' ')
        s_index_xj++;
    return (s + s_index_xj);
}

static int describe(char *line) {
    char *p = line;
    int p_index_xj = 0;
    while (p[p_index_xj] && p[p_index_xj] != '=')
        p_index_xj++;
    if (!p[p_index_xj])
        return -1;
    return (int)((p + p_index_xj) - line) * 100 + (int)strlen((p + p_index_xj));
}

int main(void) {
    char text[] = "   key=value";
    char *body = skip_spaces(text);
    printf("%d %d\n", (int)(body - text), describe(body));
    return 0;
}
