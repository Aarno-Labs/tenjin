#include <stdio.h>
#include <sys/stat.h>

long size_of(const char *path) {
    struct stat s;
    if (stat(path, &s) != 0) return -1;
    return s.st_size;
}

int main(void) {
    printf("%ld\n", size_of("/dev/null"));
    return 0;
}
