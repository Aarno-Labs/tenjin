#include <stdio.h>
#include <string.h>

/* The pointer's value is returned, subtracted, and passed to strlen. */
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
