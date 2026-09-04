/*
// When Tenjin compiles a codebase for translation, it arranges to have this file
// prepended to each translation unit. The declarations below exist so that we can
// rewrite incoming C code to refer to synthetic marker functions which can then be
// easily recognized by the core Tenjin translator.

// assert() is usually a macro defined by <assert.h>
// The converted Rust code translates the condition as a boolean, but on the C side
// it should be treated as an integral type. It must be `_Bool` instead of `int` to
// accomodate pointer-typed expressions.
*/
void assert(_Bool);

/*
// The FD_* operations are usually macros defined by <sys/select.h>.  Use void
// pointers here because this header is included before the application's own
// headers define fd_set.  c2rust redirects these markers to typed Rust helpers.
*/
void FD_ZERO(void *);
void FD_SET(int, void *);
void FD_CLR(int, void *);
int FD_ISSET(int, const void *);
