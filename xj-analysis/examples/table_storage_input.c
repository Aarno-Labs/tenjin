/* The input side of the `table->storage` example: what the pointer pass
 * sees today, before anything has been rewritten.
 *
 * `p` is a moving cursor whose base is the lvalue path `t->storage`. Today
 * xj-prepare-pointertransform recognises that textually, via
 * BaseMutationFinder, and *collapses* the cursor: `p` is deleted and its
 * accesses are rewritten against the base. See README.md for the recorded
 * metadata and the rewritten source.
 *
 * That collapse is the behaviour PR 3 relocates into this library. It is
 * why memory reasoning is not a new capability here — it is what keeps an
 * existing one.
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
