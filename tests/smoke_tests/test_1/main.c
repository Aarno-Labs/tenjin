/* #include <stdio.h> */
/* CodeHawk-C uses CIL which cannot parse Mac system headers.
    For now we avoid the issue by using a forward declaration
    instead of a real #include */
extern void printf(const char *format, ...);

int main(int argc, char **argv)
{
  for (int i = 0; i < argc; i++)
  {
    printf("Arg %d: %s\n", i, argv[i]);
  }
  return 0;
}