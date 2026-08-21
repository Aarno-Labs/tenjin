#include <stdio.h>

/* `dst` captures a struct field by value and the field is advanced
 * afterwards. Substituting the base's *source text* at every access
 * re-read the field after it moved, so this shape used to be rejected
 * outright. A retained pointer captures the value, exactly as the
 * original declaration did, and the question never comes up. */

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
