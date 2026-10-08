#include <stdio.h>

/* Writes through a pointer cast from a const parameter. */
static void patch(const char *tmpl, int n) {
    char *w = (char *)tmpl;
    for (int i = 0; i < n; i++)
        *w++ = 'x';
}

int main(void) {
    char buf[6] = "abcde";
    patch(buf, 3);
    printf("%s\n", buf);
    return 0;
}
