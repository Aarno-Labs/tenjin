/* The same loop before any rewriting: `p` is a cursor whose base is the
 * lvalue path `t->storage`.
 */

struct table
{
    char *storage;
    unsigned len;
};

unsigned checksum(struct table *t)
{
    char *p = t->storage;
    unsigned sum = 0;
    for (unsigned i = 0; i < t->len; i++)
        sum += (unsigned char)*p++;
    return sum;
}