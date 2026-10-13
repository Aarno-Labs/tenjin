// Guidance on struct fields: every use of a guided field (initializer,
// read, write, null test, index, decay to a raw pointer) is translated
// against the guided type.
#include <stdio.h>

struct Rec {
    const char *name;
    unsigned char buf[8];
    const int *vals;
    int n;
};

static const int table[3] = {1, 2, 3};

static int sum(const int *xs, int n) {
    int t = 0;
    for (int i = 0; i < n; i++) t += xs[i];
    return t;
}

static int count_char(const struct Rec *r, char c) {
    int k = 0;
    for (int i = 0; r->name[i] != '\0'; i++)
        if (r->name[i] == c) k++;
    return k;
}

int main(void) {
    struct Rec r = {"init", {0}, table, 3};
    struct Rec *p = &r;
    if (p->name == NULL) return 1;
    char first = p->name[0];
    p->name = "rewritten";
    r.buf[2] = 7;
    p->buf[3] = r.buf[2] + 1;
    int s = sum(p->vals, p->n);
    printf("%c %d %d %d %d\n", first, p->buf[3], s, p->vals[1], count_char(p, 't'));
    return 0;
}
