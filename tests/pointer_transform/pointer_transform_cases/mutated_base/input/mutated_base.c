#include <stdio.h>

/* `dst` captures the *value* of `s->out` at its declaration, but `s->out`
 * is then advanced. Substituting the base text would re-evaluate it at
 * each access and write to the wrong place, so base collapse is unsound
 * here; the frozen handle holds the captured value and is correct. */
typedef struct {
    unsigned char *out;
} Sink;

static void emit(Sink *s, const unsigned char *src, int n) {
    unsigned char *dst = s->out;
    int i = 0;
    while (i < n) {
        *dst++ = src[i];
        i++;
    }
    s->out += n;
}

int main(void) {
    unsigned char buf[8] = {0};
    const unsigned char a[3] = {1, 2, 3};
    const unsigned char b[2] = {4, 5};
    Sink s;
    s.out = buf;
    emit(&s, a, 3);
    emit(&s, b, 2);
    for (int i = 0; i < 5; i++)
        printf("%d ", buf[i]);
    printf("\n");
    return 0;
}
