#include <stdio.h>
#include <string.h>

/* The value of a pointer assignment is used in a condition. */
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
