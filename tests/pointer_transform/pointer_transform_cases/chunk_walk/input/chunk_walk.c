#include <stdio.h>

/* `p` both sweeps an array (p++) and follows links (p = p->next). The
 * reseat reads *through* `p`, so the read must be rewritten at the old
 * index before the index resets to 0. */
typedef struct Node {
    int value;
    struct Node *next;
} Node;

static int walk(Node *nodes, int n) {
    Node *p = nodes;
    int sum = 0;

    for (int i = 0; i < n; i++) {
        sum += p->value;
        p++;
    }

    p = nodes;
    while (p) {
        sum += p->value;
        p = p->next;
    }
    return sum;
}

int main(void) {
    Node ns[3];
    ns[0].value = 1;
    ns[0].next = &ns[2];
    ns[1].value = 2;
    ns[1].next = 0;
    ns[2].value = 4;
    ns[2].next = 0;
    printf("%d\n", walk(ns, 3));
    return 0;
}
