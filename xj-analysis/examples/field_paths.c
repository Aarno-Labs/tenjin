/* Bases that are field paths rather than variable names, and the rules that
 * decide when a store or a call disturbs one. `table_storage.c` is the
 * motivating program; this is the case-by-case checklist beside it, in the
 * same style as `copy_propagation.c`.
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

/* 1. Which pointer `t->storage` holds is unknown after the branch, but `p` is
   a copy of whatever it holds. */
void join_then_copy(struct table *t, char *a, char *b, int c)
{
    if (c)
        t->storage = a;
    else
        t->storage = b;
    char *p = t->storage;
    use(p); /* xj-expect: join_then_copy: p -> t->storage */
}

/* 2. `g` cannot name `x`, so it cannot name `x.buf` either. */
void non_escaping_field(void)
{
    struct box x;
    x.buf = get();
    char *p = x.buf;
    g();
    use(p); /* xj-expect: non_escaping_field: p -> x.buf */
}

/* 3. ...but `&x.buf` is handed out, so a later call may write the field. */
void escaping_field(void)
{
    struct box x;
    x.buf = get();
    char *p = x.buf;
    take(&x.buf);
    g();
    use(p); /* xj-expect: escaping_field: p -> none */
}

/* 4. `len` and `storage` are different members of the same struct, so the
   store cannot touch the base. */
void sibling_field_is_disjoint(struct table *t, unsigned n)
{
    char *p = t->storage;
    t->len = n;
    use(p); /* xj-expect: sibling_field_is_disjoint: p -> t->storage */
}

/* 5. Members of a union are the same storage, which is how type punning
   works, so the rule above must not fire. */
void union_members_alias(union pun *m, char *q)
{
    char *p = m->a;
    m->b = q;
    use(p); /* xj-expect: union_members_alias: p -> none */
}

/* 6. Assigning a whole struct writes every member of it, `x.buf` included. */
void whole_struct_assign(struct box y)
{
    struct box x;
    x.buf = get();
    char *p = x.buf;
    x = y;
    use(p); /* xj-expect: whole_struct_assign: p -> none */
}

/* 7. The same path through two different pointers: `s` and `t` may be the
   same table. */
void two_objects(struct table *s, struct table *t, char *q)
{
    char *p = t->storage;
    s->storage = q;
    use(p); /* xj-expect: two_objects: p -> none */
}

/* 8. `o->in->buf` is deeper than `MaxPathDepth`, so the store cannot be named
   and could be to anything. */
void over_depth_cap(struct outer *o, struct table *t, char *q)
{
    char *p = t->storage;
    o->in->buf = q;
    use(p); /* xj-expect: over_depth_cap: p -> none */
}

/* 9. `*q` could be `t->storage`. */
void store_through_pointer(struct table *t, char **q)
{
    char *p = t->storage;
    *q = 0;
    use(p); /* xj-expect: store_through_pointer: p -> none */
}

/* 10. `t = u` writes no byte of the old `*t`, but `t->storage` now names a
   field of a different object, so `p` no longer agrees with it.
   `denotationDependsOn` is the test for this; overlap alone would miss it. */
void reassign_base(struct table *t, struct table *u)
{
    char *p = t->storage;
    t = u;
    use(p); /* xj-expect: reassign_base: p -> none */
}

/* 11. The converse: writing through `t` does not change `t` itself. */
void store_through_pointer_keeps_root(struct table *s, unsigned n)
{
    struct table *t = s;
    t->len = n;
    use2(t); /* xj-expect: store_through_pointer_keeps_root: t -> s */
}

/* 12. ...unless `t` points at itself, in which case its first member is `t`
   and `t->storage = q` does write it. Building such a pointer needs `&t`,
   which is what the test for it looks at.

   This program violates strict aliasing, and the analysis still handles it:
   tenjin translates C that is routinely built -fno-strict-aliasing. */
void self_pointing(char *q)
{
    struct table *t = (struct table *)&t;
    struct table *u = t; /* class {t, u} */
    t->storage = q;      /* writes *t, which is t */
    use2(u);             /* xj-expect: self_pointing: u -> none */
}
