#include <stdio.h>

/* A block-scope `extern` declares no pointer of its own: it names the one
 * at file scope, which is left alone here — `stream` for its linkage,
 * `cursor` for being declared twice. */

char *stream;
static char *cursor;

static void advance(void) {
    extern char *stream;
    extern char *cursor;
    stream++;
    cursor++;
}

static int peek(void) {
    cursor++;
    return *stream * 1000 + *cursor;
}

int main(void) {
    static char t[] = "abc";
    static char u[] = "xyz";
    stream = t;
    cursor = u;
    advance();
    printf("%d\n", peek());
    return 0;
}
