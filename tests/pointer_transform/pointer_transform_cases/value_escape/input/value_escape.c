#include <stdio.h>
#include <string.h>

/* The pointer's value is returned, subtracted, and passed to strlen. */
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
