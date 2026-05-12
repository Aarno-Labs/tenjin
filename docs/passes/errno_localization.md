# Errno Localization

## Where

- [xj-localize-errno](/xj-localize-errno)

## What

C preparatory refactoring pass which converts function calls that may set the global `errno` into wrapper calls manipulating a local variable.

## Why

`errno` is a mutable global variable, which is not compatible with safe Rust.
Converting `errno`-based code into safe and idiomatic Rust equivalents is not trivial.
We perform localization of globals already, but errno is a special case that can get
better-than-baseline treatment.

Almost everything in libc *may* set errno; virtually nothing *must*.
But: errno should be (is generally?) only read after having been locally written.

## Examples

Before:
```c
void g(char *n) {
	FILE *f = fopen(n);
    if (f && errno == EACCES) { /* ... */ }
}
```

After:
```c
void fopen_wrap(char *p, int *error) {
	FILE *res = fopen(p);
	*error = errno;
}

void g(char *n) {
    int error = 0;
	FILE *f = fopen_wrap(n, &error);
    if (f && errno == EACCES) { /* ... */ }
}
```

## How

- CodeHawk analysis determines whether all observations of errno return values that
do not depend on its value on entry.

## Other Notes

- Subsequent passes will eventually convert the wrappers to return Result-equivalent
structs.
- Sloppy C code may observe errno values tainted from the execution context:
```c
void g(char *n) {
	FILE *f = fopen(n);
    if (errno == EACCES) { /* ... */ }
    // fopen only writes errno when it returns NULL
}
```

