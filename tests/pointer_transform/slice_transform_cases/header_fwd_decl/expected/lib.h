#ifndef LIB_H
#define LIB_H

typedef struct { int *ptr; size_t len; } RustSlice_int;

int sum(RustSlice_int arr);

#endif
