const char *shared(void) { return "shared"; }
char *mutable(void) { return "mutable"; }
const signed char *signed_bytes(void) { return (const signed char *)"signed"; }
const char (*array_address(void))[6] { return &"array"; }
