#include <stdio.h>
#include <string.h>

static int strchr_index_xj(const char *base, int start, int c) {
    const char *result = strchr(base + start, c);
    if (!result) return -1;
    return (int)(result - base);
}

static int count_commas(const char *s) {
    int p_index_xj = 0;
    int count = 0;
    while (1) {
        p_index_xj = strchr_index_xj(s, p_index_xj, ',');
        if (p_index_xj == -1)
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
