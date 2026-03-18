#include "bar.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

__attribute__((external_source_symbol(language="C",defined_in="foo.h")))
int foo(void)
{
  return 0;
}

int main(int argc, char **argv)
{
    if (foo() == 0 && bar() == 1 && errno == EINVAL) {
        printf("LOL: [%s]\n", strerror(errno));
    }
}
