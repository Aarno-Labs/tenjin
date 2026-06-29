/* NEGATIVE: field `y` is never written, so the struct is not fully
 * initialized. Expect: left completely untouched. */
#include <stdlib.h>

struct Point {
    int x;
    int y;
};

extern void use(struct Point *p);

void neg_partial_init(int a) {
    struct Point *p = malloc(sizeof(struct Point));
    p->x = a;
    use(p);
}
