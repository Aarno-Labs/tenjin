/* POSITIVE: canonical malloc + null-check + full init at the declaration site.
 * Expect: null-check removed, fields hoisted to temporaries, stack value built,
 * `struct Point *p = box__new(&_xj_p_stack, sizeof(_xj_p_stack));`. */
#include <stdlib.h>

struct Point {
    int x;
    int y;
};

extern void use(struct Point *p);

void pos_decl_at_site(int a, int b) {
    struct Point *p = malloc(sizeof(struct Point));
    if (!p) {
        return;
    }
    p->x = a;
    p->y = b + 1;
    use(p);
}
