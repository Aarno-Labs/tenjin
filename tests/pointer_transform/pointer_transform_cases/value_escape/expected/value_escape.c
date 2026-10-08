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
    int p_index_xj = 0;
    while (line[p_index_xj] && line[p_index_xj] != '=')
        p_index_xj++;
    if (!line[p_index_xj])
        return -1;
    return (int)(p_index_xj) * 100 + (int)strlen((line + p_index_xj));
}

int main(void) {
    char text[] = "   key=value";
    char *body = skip_spaces(text);
    printf("%d %d\n", (int)(body - text), describe(body));
    return 0;
}
