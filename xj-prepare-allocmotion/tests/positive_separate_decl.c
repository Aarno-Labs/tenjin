/* POSITIVE: separately-declared pointer, calloc, brace-less null-check.
 * Expect: rewritten to `p = box__new(&_xj_p_stack, sizeof(_xj_p_stack));`
 * (the earlier `struct Point *p;` declaration is left in place). */
#include <stdlib.h>

struct Point {
    int x;
    int y;
};

extern void use(struct Point *p);

void pos_separate_decl(int a, int b) {
    struct Point *p;
    p = calloc(1, sizeof(struct Point));
    if (p == NULL)
        return;
    p->x = a;
    p->y = b;
    use(p);
}
