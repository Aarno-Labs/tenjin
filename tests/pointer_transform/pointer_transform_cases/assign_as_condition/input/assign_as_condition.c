#include <stdio.h>
#include <string.h>

/* A pointer assignment that is itself the truth test. */

/* Loop condition, in the conventional extra parentheses. */
static int count_fields(const char *s) {
    const char *p = s;
    int n = 0;
    while ((p = strchr(p, ','))) {
        n++;
        p++;
    }
    return n;
}

/* Operand of &&. */
static int count_up_to(const char *s, int limit) {
    const char *p = s;
    int n = 0;
    while (n < limit && (p = strchr(p, ','))) {
        n++;
        p++;
    }
    return n;
}

/* if condition. */
static int offset_after_first(const char *s, const char *needle) {
    const char *p = s;
    p++;
    if ((p = strstr(p, needle)))
        return (int)(p - s);
    return -1;
}

int main(void) {
    printf("%d %d %d %d\n", count_fields("a,bb,ccc,"), count_up_to("a,bb,ccc,", 2),
           offset_after_first("hello world", "wor"),
           offset_after_first("hello world", "xyz"));
    return 0;
}
