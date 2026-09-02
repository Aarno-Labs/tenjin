/* Why bases that are field paths matter. Each function walks a buffer with a
 * local cursor, and they differ only in where the cursor's base comes from.
 * That difference decides whether the pointer transform can delete the cursor
 * and index off its base instead, or has to leave it as a moving pointer.
 */

struct table
{
    char *storage;
    unsigned len;
};

extern void log_start(void);

/* The base is a parameter's name. This is the only shape slicing can use:
   `detectRoots` accepts a base only when it is a pointer parameter. */
unsigned checksum_named(char *buf, unsigned len)
{
    char *p = buf;
    unsigned sum = 0;
    for (unsigned i = 0; i < len; i++)
        sum += (unsigned char)p[i]; /* xj-expect: checksum_named: p -> buf */
    return sum;
}

/* The base is `t->storage`, which is not any variable's name. This is the
   case the library exists for. */
unsigned checksum(struct table *t)
{
    char *p = t->storage;
    unsigned sum = 0;
    for (unsigned i = 0; i < t->len; i++)
        sum += (unsigned char)p[i]; /* xj-expect: checksum: p -> t->storage */
    return sum;
}

/* The store is to a different member of the same struct, so it cannot touch
   the base. */
unsigned checksum_and_reset(struct table *t)
{
    char *p = t->storage;
    unsigned sum = 0;
    for (unsigned i = 0; i < t->len; i++)
        sum += (unsigned char)p[i]; /* xj-expect: checksum_and_reset: p -> t->storage */
    t->len = 0;
    return sum;
}

/* `t` points at an object the caller can reach, so `log_start` may reach it
   too — through a global, say — and assign `t->storage`. */
unsigned checksum_logged(struct table *t)
{
    char *p = t->storage;
    unsigned sum = 0;
    log_start();
    for (unsigned i = 0; i < t->len; i++)
        sum += (unsigned char)p[i]; /* xj-expect: checksum_logged: p -> none */
    return sum;
}
