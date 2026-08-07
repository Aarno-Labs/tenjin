#include <stdio.h>

/* Collapse mode, two declarators in a for-init. The index declarations
 * cannot go after the DeclStmt — that slot is the loop condition — and
 * the statement cannot be replaced in place either, because the other
 * declarator has to survive. Both indices are placed before the loop.
 *
 * Each initializer is self-contained (`buf`, `buf + 1`), so neither has
 * to stay inside the statement and the order they land in is free. */
static int pairwise(int *buf, int n) {
    int *end = buf + n;
    int s = 0;
    int q_index_xj = 1;
    int p_index_xj = 0;
    for (int *p = buf, *q = buf + 1; p_index_xj < (end - buf); p_index_xj++, q_index_xj++)
        s += buf[p_index_xj] * 2 + buf[q_index_xj];
    return s;
}

/* A multi-declarator at statement level keeps the existing placement:
 * there is a position after the declaration, so the indices go there. */
static int statement_level(int *buf, int n) {
    int s = 0;
    int *p = buf, *q = buf + 1;
    int p_index_xj = 0;
    int q_index_xj = 1;
    while (n-- > 1) {
        s += buf[p_index_xj] + buf[q_index_xj] * 3;
        p_index_xj++;
        q_index_xj++;
    }
    return s;
}

/* A pointer declared in a loop body is re-initialized on every
 * iteration, so its index has to restart with it. */
static int reinit_each_pass(int *buf, int n) {
    int s = 0;
    for (int i = 0; i < n; i++) {
        int *p = buf + i, *q = buf;
        int p_index_xj = i;
        int q_index_xj = 0;
        s += buf[p_index_xj] - buf[q_index_xj];
        p_index_xj++;
        q_index_xj++;
        s += buf[p_index_xj] - buf[q_index_xj];
    }
    return s;
}

int main(void) {
    int buf[8] = {1, 2, 4, 8, 16, 32, 64, 128};
    printf("%d %d %d\n", pairwise(buf, 4), statement_level(buf, 4),
           reinit_each_pass(buf, 3));
    return 0;
}
