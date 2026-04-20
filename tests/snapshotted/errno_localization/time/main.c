#include <time.h>
#include <errno.h>

int main(int argc, char **argv)
{
    errno = 0;
    time_t t;
    time(&t);
    if (errno == 0)
    {
        return 0;
    }
    return 1;
}
