/* NEGATIVE: the pointer's address is taken (`&p`), so it may be aliased and
 * we cannot safely move the allocation. Expect: left untouched. */
#include <stdlib.h>

struct Point {
    int x;
    int y;
};

extern void use(struct Point *p);

void neg_aliased(int a, int b) {
    struct Point *p = malloc(sizeof(struct Point));
    struct Point **q = &p;
    p->x = a;
    p->y = b;
    use(*q);
}
