#include <stdio.h>

typedef struct {
    int value;
} Item;

static Item items[2] = {{41}, {42}};

static int find_item(int index);

static int find_item(int index) {
    if (index >= 0 && index < 2)
        return index;
    return -1;
}

static int use_item(Item *item) {
    return item->value;
}

static int item_exists(int index) {
    return find_item(index) != -1;
}

static int get_item_value(int index) {
    int item = find_item(index);
    if (item != -1)
        return use_item(&items[item]);
    return 0;
}

int main(void) {
    printf("%d %d %d %d\n", get_item_value(0), get_item_value(1), get_item_value(5),
           item_exists(1));
    return 0;
}
