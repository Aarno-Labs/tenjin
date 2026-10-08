#include <stdio.h>
#include <string.h>

/* A search through a file-scope pointer needs its wrapper defined ahead
 * of the function the search is in. The pointer itself is in none. */

static char *cursor;
static int cursor_index_xj = 0;

static int strstr_index_xj(const char *base, int start, const char *needle) {
    const char *result = strstr(base + start, needle);
    if (!result) return -1;
    return (int)(result - base);
}

static int count_pairs(char *s) {
    char *p = s;
    int p_index_xj = 0;
    int n = 0;
    while (((p_index_xj = strstr_index_xj(p, p_index_xj, "ab")) >= 0)) {
        n++;
        p_index_xj++;
    }
    return n;
}

static int strchr_index_xj(const char *base, int start, int c) {
    const char *result = strchr(base + start, c);
    if (!result) return -1;
    return (int)(result - base);
}

static int count(char c) {
    int n = 0;
    while (((cursor_index_xj = strchr_index_xj(cursor, cursor_index_xj, c)) >= 0)) {
        n++;
        cursor_index_xj++;
    }
    return n;
}

static int count_again(char *s) {
    cursor = s, cursor_index_xj = 0;
    cursor_index_xj = strstr_index_xj(cursor, cursor_index_xj, "ab");
    return (cursor && cursor_index_xj >= 0) ? (int)((cursor_index_xj < 0 ? (void *)0 : cursor + cursor_index_xj) - s) : -1;
}

int main(void) {
    static char t[] = "a,b,c,d";
    char u[] = "xxababab";
    cursor = t, cursor_index_xj = 0;
    printf("%d %d %d\n", count(','), count_pairs(u), count_again(u));
    return 0;
}
