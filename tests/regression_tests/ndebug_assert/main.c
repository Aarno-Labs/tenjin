#include <assert.h>
#include <stdio.h>

int main(int argc, char **argv) {
    // With NDEBUG defined, the argument of assert() is never parsed, so it
    // need not even refer to declared names. Earlier Tenjin stages can leave
    // assert arguments in this state when they rewrite the code around them.
    assert(no_such_variable > 0);
    // This would fail if asserts were (wrongly) translated.
    assert(argc == 99);
    printf("ok\n");
    return 0;
}
