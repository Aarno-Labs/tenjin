#include <assert.h>
#include <unistd.h>

int isatty_stdout() { return isatty(STDOUT_FILENO); }
int isatty_stderr() { return isatty(STDERR_FILENO); }
int isatty_stdin() { return isatty(STDIN_FILENO); }

extern int puts(const char*);
void string_cond_1(int cond) { puts(cond ? "true" : "false"); }

void assert_plain(int x) { assert(x > 0); }
void assert_msg(int x) { assert(x > 0 && "x must be positive"); }
void assert_msg_chained(int x, int y) { assert(x > 0 && y > 0 && "both must be positive"); }
void assert_msg_braces(int x) { assert(x > 0 && "x must not be {0}"); }
