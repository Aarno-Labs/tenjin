#include <set>
#include "StdlibSpec.h"

const std::set<std::string> ERRNO_UNMODIFIED = {
    // <ctype.h>
    "isalnum",
    "isalpha",
    "isascii",
    "isblank",
    "iscntrl",
    "isdigit",
    "isgraph",
    "islower",
    "isprint",
    "ispunct",
    "isspace",
    "isupper",
    "isxdigit",
    "isalnum_l",
    "isalpha_l",
    "isascii_l",
    "isblank_l",
    "iscntrl_l",
    "isdigit_l",
    "isgraph_l",
    "islower_l",
    "isprint_l",
    "ispunct_l",
    "isspace_l",
    "isupper_l",
    "isxdigit_l",
    "toupper",
    "tolower",
    "toupper_l",
    "tolower_l",
    // <stdio.h>
    // formatted output conversion
    "printf",
    "fprintf",
    "dprintf",
    "sprintf",
    "snprintf",
    "vprintf",
    "vfprintf",
    "vdprintf",
    "vsprintf",
    "vsnprintf",
    // input format conversion,
    "fscanf",
    "scanf",
    "sscanf",
    "vfscanf",
    "vscanf",
    "vsscanf",
    // output of characters and strings
    "fputc",
    "fputs",
    "putc",
    "putchar",
    "puts",
    // <stdlib.h>
    // TODO: Regarding memory allocators, POSIX and the C standard
    // say different things about when errno should be set
    "free",
    "exit",
    // <string.h>
    "memset",
    "strlen",
    "strspn",
    "strcspn",
};

bool NeedsWrapper(clang::FunctionDecl *Decl)
{
  return ERRNO_UNMODIFIED.find(Decl->getNameAsString()) == ERRNO_UNMODIFIED.end();
}