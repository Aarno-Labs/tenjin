/* A file-scope pointer with external linkage, walked with pointer
 * arithmetic in the translation unit that defines it and read through an
 * `extern` declaration in another one.
 *
 * Splitting it into a base plus an index is unsound: this pass runs per
 * translation unit, so main.c keeps its `extern const char **cursor;`
 * and keeps spelling `*cursor`, with no `cursor_index_xj` to advance.
 * The base pointer still exists, so the program would link and quietly
 * print the first entry instead of the one `pick` selected. An
 * externally visible file-scope pointer has to be left alone. */

static const char *table[] = {"alpha", "beta", "gamma", 0};

const char **cursor;

void pick(const char *want) {
    for (cursor = table; *cursor; ++cursor)
        if (want[0] == (*cursor)[0]) return;
}
