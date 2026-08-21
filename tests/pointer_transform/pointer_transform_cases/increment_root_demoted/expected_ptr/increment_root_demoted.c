#include <stdio.h>

/* q = p++ where q is not rewritten because its address is taken. */

static void bump(char **pp) { (*pp)++; }

static int scan(char *p) {
    int p_index_xj = 0;
    char *q = (p + p_index_xj++);
    bump(&q);

    int n = 0;
    while (p[p_index_xj]) {
        n += 1;
        p_index_xj++;
    }
    return n + (int)((p + p_index_xj) - q);
}

int main(void) {
    char text[] = "abcde";
    printf("%d\n", scan(text));
    return 0;
}
