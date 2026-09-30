#include <stdio.h>

/* q = p++ where q is not rewritten because its address is taken. */

static void bump(char **pp) { (*pp)++; }

static int scan(char *p) {
    char *q = p++;
    bump(&q);

    int n = 0;
    while (*p) {
        n += 1;
        p++;
    }
    return n + (int)(p - q);
}

int main(void) {
    char text[] = "abcde";
    printf("%d\n", scan(text));
    return 0;
}
