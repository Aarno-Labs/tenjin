#include <stdbool.h>

int main() {
    // Using _Bool type directly
    _Bool isAdmin = 1;

    // Using bool type from stdbool.h
    bool isSubscriber = true;

    // Conditional statement with _Bool
    _Bool isValid = 0;
    if (!isValid && isSubscriber && isAdmin) return 1;
    return 0;
}
