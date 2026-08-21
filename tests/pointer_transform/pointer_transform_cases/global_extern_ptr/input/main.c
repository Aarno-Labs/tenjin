#include <stdio.h>

extern const char **cursor;
void pick(const char *want);

int main(void) {
    pick("gamma");
    printf("%s\n", *cursor);
    return 0;
}
