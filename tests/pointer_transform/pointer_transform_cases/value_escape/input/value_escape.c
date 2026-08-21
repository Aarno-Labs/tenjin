#include <stdio.h>
#include <string.h>

/* Uses of the pointer's value that are not element accesses: returned,
 * differenced against its origin, handed to a library function. Each is
 * rebuilt in place as `(p + p_index_xj)`, which is the fallback that
 * lets the rewrite be total instead of declining what it cannot index. */
static char *skip_spaces(char *s) {
    while (*s == ' ')
        s++;
    return s;
}

static int describe(char *line) {
    char *p = line;
    while (*p && *p != '=')
        p++;
    if (!*p)
        return -1;
    return (int)(p - line) * 100 + (int)strlen(p);
}

int main(void) {
    char text[] = "   key=value";
    char *body = skip_spaces(text);
    printf("%d %d\n", (int)(body - text), describe(body));
    return 0;
}
