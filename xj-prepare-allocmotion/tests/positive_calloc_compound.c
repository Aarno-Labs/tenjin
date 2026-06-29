/* POSITIVE (the cp_inflate-shaped case): calloc + repeated writes + a
 * compound-assign in a loop + a field-to-field read + a field left zero by
 * calloc. Expect: per-field temporaries (zero-initialized), all accesses
 * redirected, and a single designated initializer that lists `bits`/`acc`/
 * `lo`/`hi` from temporaries and zero-fills the untouched `tag` and `pad[]`. */
#include <stdlib.h>

struct Acc {
    unsigned long bits;
    int acc;
    int lo;
    int hi;
    int tag;       /* never written -> zero from calloc */
    int pad[4];    /* array, never accessed -> {0} */
};

extern void use(struct Acc *a);

void build(const unsigned char *in, int n) {
    struct Acc *a = (struct Acc *)calloc(1, sizeof(struct Acc));
    a->acc = 0;
    for (int i = 0; i < n; i++)
        a->bits |= (unsigned long)in[i] << (i * 8);   /* compound-assign in loop */
    a->acc = n * 8;                                    /* second write to acc */
    a->lo = n & 3;
    a->hi = a->lo + n;                                 /* field-to-field read */
    use(a);
}
