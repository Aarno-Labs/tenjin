#ifndef API_H_
#define API_H_

// Functions wrapped by `ffi` guidance. Declared here so that callers in either
// source file can invoke them across translation units.
unsigned char first_byte(const char *s);
void zero_first(unsigned char *buf, int n);
int sum_n(int *xs, int n);
int bump(int *p);

// Plain (un-wrapped) functions that call the wrapped ones internally.
int strings_demo(void);
int numbers_demo(void);

#endif // API_H_
