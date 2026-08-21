#include <stdio.h>

/* Two functions in one file, each with two pointers into one parameter. */

typedef struct {
    int id;
    int value;
} Entry;

static int find_id(Entry *entries, int count, int target) {
    Entry *ptr = entries;
    Entry *end = entries + count;

    while (ptr < end) {
        if (ptr->id == target)
            return ptr->value;
        ptr++;
    }
    return -1;
}

static int scale_all(Entry *entries, int count, int multiplier) {
    Entry *current;
    Entry *last;
    int total = 0;

    current = entries;
    last = entries + count;

    while (current < last) {
        current->value = current->value * multiplier;
        total += current->value;
        current++;
    }
    return total;
}

int main(void) {
    Entry e[3] = {{1, 10}, {2, 20}, {3, 30}};
    printf("%d %d ", find_id(e, 3, 2), find_id(e, 3, 9));
    printf("%d %d\n", scale_all(e, 3, 3), e[2].value);
    return 0;
}
