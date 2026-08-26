/* Why the domain needs memory reasoning — the `table->storage` shape.
 *
 * Each function below walks a buffer with a local cursor. They differ only
 * in where the cursor's base comes from, and that difference decides
 * whether the pointer transform can collapse the cursor onto its base (so
 * the translation lowers to an index into a slice) or has to keep it as a
 * frozen handle (`*mut c_char` + `.offset()`).
 *
 * `checksum_named` needs nothing beyond copy propagation over variables.
 * `checksum` is the case this library exists for: its base is an lvalue
 * path through a parameter, so with variables-only cells there is no
 * candidate for `p` to resolve to and the cursor does not collapse. Today
 * that collapse happens in the pointer pass, textually, via
 * BaseMutationFinder — so losing it is a regression, not a missing
 * feature.
 *
 * The last two mark the boundary, which is worth having in the same file
 * as the motivating case: a store to a sibling field is provably not a
 * write to the base, and an opaque call is.
 *
 * The annotations are what *this library* must produce. The M0 spike can
 * run the file (see README.md) and agrees on the first three, but reports
 * `t->storage` for `checksum_logged`, because it has no clobber rule and
 * so invalidates nothing at a call. That disagreement is the rule M1 adds,
 * not a disagreement about the example.
 */

struct table
{
    char *storage;
    unsigned len;
};

extern void log_start(void);

/* Tier 0/1. The base is a parameter's name, so a variables-only cell
   alphabet already resolves it. This is the only shape slicing can use:
   `detectRoots` accepts a base only when it equals a pointer parameter's
   name. */
unsigned checksum_named(char *buf, unsigned len)
{
    char *p = buf;
    unsigned sum = 0;
    for (unsigned i = 0; i < len; i++)
        sum += (unsigned char)p[i]; /* xj-expect: checksum_named: p -> buf */
    return sum;
}

/* Tier 2, and the point of the file. `p`'s base is `t->storage`, which is
   not any variable's name, so it is a candidate only once cells are access
   paths. With `--no-memory-candidates` this resolves to nothing and the
   cursor lowers as a raw pointer. */
unsigned checksum(struct table *t)
{
    char *p = t->storage;
    unsigned sum = 0;
    for (unsigned i = 0; i < t->len; i++)
        sum += (unsigned char)p[i]; /* xj-expect: checksum: p -> t->storage */
    return sum;
}

/* Still resolves, and this is capability the framework route could not
   have had. `t->len` and `t->storage` share the prefix `Var(t) . Deref`
   and then diverge at two distinct members of a struct, so the store
   provably does not write the base — a fact about C's object model, not an
   assumption. A model whose cells are opaque locations has to clear the
   base here. */
unsigned checksum_and_reset(struct table *t)
{
    char *p = t->storage;
    unsigned sum = 0;
    for (unsigned i = 0; i < t->len; i++)
        sum += (unsigned char)p[i]; /* xj-expect: checksum_and_reset: p -> t->storage */
    t->len = 0;
    return sum;
}

/* Does not resolve, soundly. `t` is a parameter, so the object it points
   at is reachable from the caller, therefore possibly from a global,
   therefore possibly from `log_start` — which could assign `t->storage`
   without taking an argument. Substituting `t->storage` at the use would
   be a miscompilation. The equality survives only in a call-free window,
   and measuring how often the corpus has that shape is what the M3 oracle
   run is for. */
unsigned checksum_logged(struct table *t)
{
    char *p = t->storage;
    unsigned sum = 0;
    log_start();
    for (unsigned i = 0; i < t->len; i++)
        sum += (unsigned char)p[i]; /* xj-expect: checksum_logged: p -> none */
    return sum;
}
