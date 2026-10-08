#include <stddef.h>
#include <stdio.h>

/* A file-scope pointer that starts out null. */
static const char *cursor = NULL;
static int cursor_index_xj = -1;

static void start(const char *s) { cursor = s, cursor_index_xj = 0; }

static int next(void) {
    if (!cursor || !cursor[cursor_index_xj])
        return -1;
    return cursor[cursor_index_xj++];
}

static const char *where(void) { return (cursor ? cursor + cursor_index_xj : (void *)0); }

int main(void) {
    int before = next();
    int null_before = where() == NULL;
    start("ab");
    int a = next();
    int b = next();
    int c = next();
    printf("%d %d %d %d %d %d\n", before, null_before, a, b, c, where() != NULL);
    return 0;
}
