/* A pointer with external linkage, read from another file, is left alone. */

static const char *table[] = {"alpha", "beta", "gamma", 0};

const char **cursor;

void pick(const char *want) {
    for (cursor = table; *cursor; ++cursor)
        if (want[0] == (*cursor)[0]) return;
}
