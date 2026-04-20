#include <stdio.h>

int lib();
int foo();

int main()
{
  printf("Hello, Tenjin! %d %d\n", lib(), foo());
  return 0;
}
