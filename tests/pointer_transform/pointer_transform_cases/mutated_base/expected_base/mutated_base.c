#include <stdio.h>

/* The field that initializes dst is modified afterwards. */

struct sink {
    char store[16];
    char *out;
};

static void emit(struct sink *s, const char *msg, int n) {
    char *dst = s->out;
    int dst_index_xj = 0;
    for (int i = 0; i < n; i++)
        dst[dst_index_xj++] = msg[i];
    s->out += n;
}

int main(void) {
    struct sink s;
    s.out = s.store;
    emit(&s, "abc", 3);
    emit(&s, "de", 2);
    *s.out = '\0';
    printf("%s %d\n", s.store, (int)(s.out - s.store));
    return 0;
}
