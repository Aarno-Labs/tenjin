#include <stddef.h>
#include <stdio.h>

/* A file-scope pointer that starts out null. */
static const char *cursor = NULL;

static void start(const char *s) { cursor = s; }

static int next(void) {
    if (!cursor || !*cursor)
        return -1;
    return *cursor++;
}

static const char *where(void) { return cursor; }

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
