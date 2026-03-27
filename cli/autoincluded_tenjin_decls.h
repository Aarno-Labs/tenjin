// When Tenjin compiles a codebase for translation, it arranges to have this file
// prepended to each translation unit. The declarations below exist so that we can
// rewrite incoming C code to refer to synthetic marker functions which can then be
// easily recognized by the core Tenjin translator.

// assert() is usually a macro defined by <assert.h>
// The converted Rust code translates the condition as a boolean, but on the C side
// it should be treated as an integral type.
void assert(int);
extern int errno;