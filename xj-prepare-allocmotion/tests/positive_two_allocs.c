/* POSITIVE: two independent allocations in one function, each fully
 * initialized before its first use. Expect: both rewritten, and the
 * box__new helper emitted exactly once at the top of the file. */
#include <stdlib.h>

struct P {
    int x;
    int y;
};

extern void use(struct P *);

void two_allocs(int a, int b, int c, int d) {
    struct P *p = malloc(sizeof(struct P));
    if (!p)
        return;
    p->x = a;
    p->y = b;

    struct P *q = malloc(sizeof(struct P));
    q->x = c;
    q->y = d;

    use(p);
    use(q);
}
