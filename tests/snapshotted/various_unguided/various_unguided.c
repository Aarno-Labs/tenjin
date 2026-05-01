#include <unistd.h>

int isatty_stdout() { return isatty(STDOUT_FILENO); }
int isatty_stderr() { return isatty(STDERR_FILENO); }
int isatty_stdin() { return isatty(STDIN_FILENO); }

extern int puts(const char*);
void string_cond_1(int cond) { puts(cond ? "true" : "false"); }