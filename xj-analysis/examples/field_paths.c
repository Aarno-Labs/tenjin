/* M2: the alphabet is access paths, and §2's memory reasoning starts to
 * carry weight. `table_storage.c` is the motivating shape; this file is the
 * rule-by-rule checklist beside it, in the same style as
 * `copy_propagation.c` — one mechanism per function, each the smallest C
 * that exercises it.
 *
 * Everything here reports `none` under `--no-memory-candidates`, since
 * without field-path cells there is no candidate to name and no store to
 * rule disjoint. That is the point of the file: every answer below is a
 * consequence of M2, and half of them are answers M2 must be careful *not*
 * to get wrong.
 */

extern void use(char *);
extern void take(char **);
extern char *get(void);
extern void g(void);

struct table
{
    char *storage;
    unsigned len;
};

extern void use2(struct table *);

struct box
{
    char *buf;
};

struct inner
{
    char *buf;
};

struct outer
{
    struct inner *in;
};

union pun
{
    char *a;
    char *b;
};

/* 1. The join case, which is the headline of partition_domain_design.md §0
   with a field path in place of a variable. Neither branch's value survives
   the join, but `t->storage` survives as a *class*, so the copy has
   something to attach to and the equality is recovered. Under the symbol
   domain both cells were T and the copy recorded nothing. */
void join_then_copy(struct table *t, char *a, char *b, int c)
{
    if (c)
        t->storage = a;
    else
        t->storage = b;
    char *p = t->storage;
    use(p); /* xj-expect: join_then_copy: p -> t->storage */
}

/* 2. A call cannot reach a field of a non-escaping local: nothing outside
   the function can name `x`, so nothing outside it can name `x.buf`. This
   is the field-path half of `call_does_not_kill_a_local`. */
void non_escaping_field(void)
{
    struct box x;
    x.buf = get();
    char *p = x.buf;
    g();
    use(p); /* xj-expect: non_escaping_field: p -> x.buf */
}

/* 3. ...unless the field's address was taken. `&x.buf` hands out a pointer
   *into* `x`, so the callee may write it, and the escape has to attach to
   the root — a rule the M1 escape collector did not need, because with no
   field paths there was nothing between `&` and a variable. */
void escaping_field(void)
{
    struct box x;
    x.buf = get();
    char *p = x.buf;
    take(&x.buf);
    g();
    use(p); /* xj-expect: escaping_field: p -> none */
}

/* 4. Two distinct members of one struct do not overlap, so a store to the
   sibling leaves the base alone. The clause that buys everything — and the
   same one `checksum_and_reset` turns on. */
void sibling_field_is_disjoint(struct table *t, unsigned n)
{
    char *p = t->storage;
    t->len = n;
    use(p); /* xj-expect: sibling_field_is_disjoint: p -> t->storage */
}

/* 5. Two members of a *union* share storage by construction, which is where
   type punning lives. The struct clause must not fire here. */
void union_members_alias(union pun *m, char *q)
{
    char *p = m->a;
    m->b = q;
    use(p); /* xj-expect: union_members_alias: p -> none */
}

/* 6. A whole-object store writes every field of it. This is the rule that
   forces non-pointer variables into the alphabet: were `x` not a cell,
   `x = y` would be an *unresolvable* store, and T2 spares everything out of
   reach — which `x.buf` is. */
void whole_struct_assign(struct box y)
{
    struct box x;
    x.buf = get();
    char *p = x.buf;
    x = y;
    use(p); /* xj-expect: whole_struct_assign: p -> none */
}

/* 7. Same root, but the paths diverge before any field: `s->storage` and
   `t->storage` are the same path through different pointers, and nothing
   says the pointers differ. Distinctness of cells never proves
   disjointness. */
void two_objects(struct table *s, struct table *t, char *q)
{
    char *p = t->storage;
    s->storage = q;
    use(p); /* xj-expect: two_objects: p -> none */
}

/* 8. Over `MaxPathDepth`, so the destination is not nameable and the store
   is unresolvable — lossy, never unsound. */
void over_depth_cap(struct outer *o, struct table *t, char *q)
{
    char *p = t->storage;
    o->in->buf = q;
    use(p); /* xj-expect: over_depth_cap: p -> none */
}

/* 9. A store through a pointer with no cell of its own. There is no Index
   step and no bare-Deref cell, so this is the shape every unresolvable
   store reduces to. */
void store_through_pointer(struct table *t, char **q)
{
    char *p = t->storage;
    *q = 0;
    use(p); /* xj-expect: store_through_pointer: p -> none */
}

/* 10. The one an access-path alphabet must not get wrong: a cell named
   through a mutable pointer stops denoting the same storage when that
   pointer is reassigned. `t->storage` after `t = u` is a different object,
   so the equality `p` had with it cannot survive.

   This is the whole reason `denotationDependsOn` exists beside
   `mayOverlap`. Not one byte of the old `*t` is written by `t = u`, and
   the two cells do not overlap: `Var(t)` is the pointer variable,
   `Var(t).Deref.Field(storage)` is a field of somebody else's object. What
   breaks is that the path no longer *reaches* the storage the equality was
   about. `Var(t)` is a proper prefix of the path and the steps below it go
   out through a Deref, so the store moves it. */
void reassign_base(struct table *t, struct table *u)
{
    char *p = t->storage;
    t = u;
    use(p); /* xj-expect: reassign_base: p -> none */
}

/* 11. The converse of 10, and what the prefix/overlap split bought. The
   store goes *through* `t` rather than to it, so `t`'s own value is
   untouched: `t->len` is storage in the pointee, `t` is a pointer variable
   whose address is never taken, and nothing can point back into it. The
   equality `t` has with `s` survives.

   Under the old single `mayAlias` this reported `none`. The prefix case
   answered true in both directions, so the analysis was more precise about
   the fields of `*t` — case 4 — than about `t` itself. */
void store_through_pointer_keeps_root(struct table *s, unsigned n)
{
    struct table *t = s;
    t->len = n;
    use2(t); /* xj-expect: store_through_pointer_keeps_root: t -> s */
}

/* 12. ...and the guard on 11, which is why the narrowed clause asks about
   reach rather than answering false outright. `t` is punned to point at
   its own storage, so `(*t).storage` — the first member — *is* the bytes of
   `t`, and `t->storage = q` writes `t` after all. Forming that pointer
   needs `&t`, which puts `t` in reach and brings the overlap back.

   Strictly this program is a strict-aliasing violation, so one could argue
   the clause could be unconditional. The analysis declines to rely on
   that: tenjin translates C that is routinely built -fno-strict-aliasing,
   and the reach test costs one comparison. */
void self_pointing(char *q)
{
    struct table *t = (struct table *)&t;
    struct table *u = t; /* class {t, u} */
    t->storage = q;      /* writes *t, which is t */
    use2(u);             /* xj-expect: self_pointing: u -> none */
}
