#include <stdio.h>

typedef struct {
    int value;
} Item;

static Item items[2] = {{41}, {42}};

static Item *find_item(int index);

static Item *find_item(int index) {
    if (index >= 0 && index < 2)
        return &items[index];
    return (void *)0;
}

static int use_item(Item *item) {
    return item->value;
}

static int item_exists(int index) {
    return find_item(index) != (void *)0;
}

static int get_item_value(int index) {
    Item *item = find_item(index);
    if (item)
        return use_item(item);
    return 0;
}

int main(void) {
    printf("%d %d %d %d\n", get_item_value(0), get_item_value(1), get_item_value(5),
           item_exists(1));
    return 0;
}
