#include <stdio.h>
#include <string.h>

/* A search through a file-scope pointer needs its wrapper defined ahead
 * of the function the search is in. The pointer itself is in none. */

static char *cursor;

static int count_pairs(char *s) {
    char *p = s;
    int n = 0;
    while ((p = strstr(p, "ab"))) {
        n++;
        p++;
    }
    return n;
}

static int count(char c) {
    int n = 0;
    while ((cursor = strchr(cursor, c))) {
        n++;
        cursor++;
    }
    return n;
}

static int count_again(char *s) {
    cursor = s;
    cursor = strstr(cursor, "ab");
    return cursor ? (int)(cursor - s) : -1;
}

int main(void) {
    static char t[] = "a,b,c,d";
    char u[] = "xxababab";
    cursor = t;
    printf("%d %d %d\n", count(','), count_pairs(u), count_again(u));
    return 0;
}
