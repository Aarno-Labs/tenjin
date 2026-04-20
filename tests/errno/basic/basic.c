#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

int foo(void)
{
  return 0;
}

int doesnt_use_errno(FILE *f)
{
  (void)fclose(f); 
  return 0;
}

int does_use_errno(FILE *f)
{
  int r = fclose(f); 
  if (r < 0)
  {
    return errno;
  }
  return 0;
}

int main(int argc, char **argv)
{
    foo();
    errno = 0;
    if (errno == EINVAL) {
        printf("Error: [%s]\n", strerror(errno));
    }
    return 0;
}
