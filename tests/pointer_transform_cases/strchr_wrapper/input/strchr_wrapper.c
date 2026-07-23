#include <stdio.h>
#include <string.h>

static int count_commas(const char *s) {
    const char *p = s;
    int count = 0;
    while (1) {
        p = strchr(p, ',');
        if (!p)
            break;
        count++;
        p++;
    }
    return count;
}

int main(void) {
    printf("%d\n", count_commas("a,b,c,d"));
    printf("%d\n", count_commas("nothing here"));
    return 0;
}
