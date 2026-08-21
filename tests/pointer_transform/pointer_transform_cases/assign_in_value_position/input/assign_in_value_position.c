#include <stdio.h>
#include <string.h>

/* The assignment's own value is consumed, so the comma expression has to
 * hand back a pointer rather than the index it just set. */
static int count_fields(char *s) {
    char *p = s;
    int n = 0;
    while ((p = strchr(p, ',')) != NULL) {
        n++;
        p++;
    }
    return n;
}

int main(void) {
    char text[] = "a,bb,ccc,";
    printf("%d\n", count_fields(text));
    return 0;
}
