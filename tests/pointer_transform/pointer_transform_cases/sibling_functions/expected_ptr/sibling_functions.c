#include <stdio.h>

/* Two functions in one file, each reconstructing two cursors onto the
 * same parameter, and one of them declaring its cursors without an
 * initializer so the declarations are deleted rather than replaced.
 *
 * The shape is here because the edits of the *earlier* function land in
 * the same rewrite buffer as the later one's, and an implementation that
 * measures a range against that buffer rather than against the original
 * text gets a short answer for every deletion after the first — leaving
 * the tail of a declaration behind, which is a miscompilation the earlier
 * one-cursor fixtures cannot see. */

typedef struct {
    int id;
    int value;
} Entry;

static int find_id(Entry *entries, int count, int target) {
    Entry *ptr = entries;
    int ptr_index_xj = 0;
    Entry *end = entries;
    int end_index_xj = count;

    while ((ptr + ptr_index_xj) < (end + end_index_xj)) {
        if (ptr[ptr_index_xj].id == target)
            return ptr[ptr_index_xj].value;
        ptr_index_xj++;
    }
    return -1;
}

static int scale_all(Entry *entries, int count, int multiplier) {
    Entry *current;
    int current_index_xj = 0;
    Entry *last;
    int last_index_xj = 0;
    int total = 0;

    (current = entries, current_index_xj = 0);
    (last = entries, last_index_xj = count);

    while ((current + current_index_xj) < (last + last_index_xj)) {
        current[current_index_xj].value = current[current_index_xj].value * multiplier;
        total += current[current_index_xj].value;
        current_index_xj++;
    }
    return total;
}

int main(void) {
    Entry e[3] = {{1, 10}, {2, 20}, {3, 30}};
    printf("%d %d ", find_id(e, 3, 2), find_id(e, 3, 9));
    printf("%d %d\n", scale_all(e, 3, 3), e[2].value);
    return 0;
}
