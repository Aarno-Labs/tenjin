/* NEGATIVE: the pointer is used before field `y` is initialized, so the
 * writes do not all precede the first use. Expect: left untouched. */
#include <stdlib.h>

struct Point {
    int x;
    int y;
};

extern void use(struct Point *p);

void neg_use_before_init(int a, int b) {
    struct Point *p = malloc(sizeof(struct Point));
    p->x = a;
    use(p);
    p->y = b;
}
