#include <stdio.h>
#include <string.h>

static int count_commas(const char *s) {
    const char *p = s;
    int p_index_xj = 0;
    int count = 0;
    while (1) {
        (p = strchr((p + p_index_xj), ','), p_index_xj = 0);
        if (!p)
            break;
        count++;
        p_index_xj++;
    }
    return count;
}

int main(void) {
    printf("%d\n", count_commas("a,b,c,d"));
    printf("%d\n", count_commas("nothing here"));
    return 0;
}
