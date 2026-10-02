#include <stdio.h>

/* Two functions in one file, each with two pointers into one parameter. */

typedef struct {
    int id;
    int value;
} Entry;

static int find_id(Entry *entries, int count, int target) {
    int ptr_index_xj = 0;
    int end_index_xj = count;

    while ((entries + ptr_index_xj) < (entries + end_index_xj)) {
        if (entries[ptr_index_xj].id == target)
            return entries[ptr_index_xj].value;
        ptr_index_xj++;
    }
    return -1;
}

static int scale_all(Entry *entries, int count, int multiplier) {
    int current_index_xj = 0;
    int last_index_xj = 0;
    int total = 0;

    (current_index_xj = 0);
    (last_index_xj = count);

    while ((entries + current_index_xj) < (entries + last_index_xj)) {
        entries[current_index_xj].value = entries[current_index_xj].value * multiplier;
        total += entries[current_index_xj].value;
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
