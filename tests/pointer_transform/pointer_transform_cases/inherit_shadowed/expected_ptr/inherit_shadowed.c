#include <stdio.h>

/* Index inheritance where the source pointer's name is ambiguous. Two
 * pointers named `q` — the inner one shadowing the outer — and `p`
 * inherits from the inner one. Resolving the inheritance source by
 * matching `base_array_text` against pointer names picks whichever `q`
 * the map yields first, which is the outer one: `p` would then start at
 * the outer q's position rather than the inner q's, and land at buf[6]
 * instead of buf[2]. Both `q`s collapse onto `buf`, so the wrong pick
 * still compiles — it just computes a different program. */
static int f(int *buf) {
    int s = 0;

    int q_index_xj = 4;
    q_index_xj++;
    s += buf[q_index_xj];

    {
        int q_index_xj_1 = 0;
        q_index_xj_1++;
        int p_index_xj = q_index_xj_1 + (1);
        s += buf[p_index_xj];
    }

    return s;
}

int main(void) {
    int b[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    printf("%d\n", f(b));
    return 0;
}
