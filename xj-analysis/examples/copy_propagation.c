/* Copy propagation over variables. Each function is the smallest C program
 * that exercises one rule about when a copied pointer can be replaced by the
 * name it was copied from.
 */

extern void use(char *);
extern char *get(void);
extern void g(void);
extern void take(char **);

/* 1. A copy of a parameter that is never reassigned. `p` and `buf` are the
   same pointer at the use, and it is the pointer the caller passed — which is
   what `detectRoots` requires of a slicing base. */
void copy_of_param(char *buf)
{
    char *p = buf;
    use(p); /* xj-expect: copy_of_param: p -> buf */
}

/* 2. Two uses reached by the same copy. */
void two_uses(char *buf)
{
    char *p = buf;
    use(p);
    use(p); /* xj-expect: two_uses: p -> buf */
}

/* 3. `p` is `a` at the first use and `b` at the second, so no single name can
   replace it at both. */
void disagreeing_uses(char *a, char *b)
{
    char *p = a;
    use(p);
    p = b;
    use(p); /* xj-expect: disagreeing_uses: p -> none */
}

/* 4. The call's return value is not known to equal anything. */
void opaque_rhs(char *buf)
{
    char *p = buf;
    p = get();
    use(p); /* xj-expect: opaque_rhs: p -> none */
}

/* 5. `p` is `a` on one path and `b` on the other. */
void join_disagrees(char *a, char *b, int c)
{
    char *p;
    if (c)
        p = a;
    else
        p = b;
    use(p); /* xj-expect: join_disagrees: p -> none */
}

/* 6. `g` cannot name `buf` or `p`, so it cannot change either one. Passing a
   pointer parameter to a callee does not expose the parameter itself. */
void call_does_not_kill_a_local(char *buf)
{
    char *p = buf;
    g();
    use(p); /* xj-expect: call_does_not_kill_a_local: p -> buf */
}

/* 7. ...but `take` is handed `&buf` and may store through it, leaving `p`
   with the old value. */
void address_taken_escapes(char *buf)
{
    char *p = buf;
    take(&buf);
    use(p); /* xj-expect: address_taken_escapes: p -> none */
}

/* 8. `p` and `buf` agree at every use, but `&p` needs `p` to have storage of
   its own, so `p` cannot be deleted. Ignoring `&p` as a use site is not
   enough: a `use(p)` elsewhere would then license the rewrite anyway. */
void address_of_cursor(char *buf)
{
    char *p = buf;
    take(&p); /* xj-expect: address_of_cursor: p -> none */
}

/* 9. `buf` is reassigned before the use, so it names a different pointer
   there. */
void param_reassigned_before_use(char *buf, char *other)
{
    char *p = buf;
    buf = other;
    use(p); /* xj-expect: param_reassigned_before_use: p -> none */
}

/* 10. The same store, but no use of `p` follows it. A reassigned base is not
   by itself a reason to give up. */
void param_reassigned_after_use(char *buf, char *other)
{
    char *p = buf;
    use(p); /* xj-expect: param_reassigned_after_use: p -> buf */
    buf = other;
    use(buf);
}

/* 11. `p` equals `buf` at the use, but not the pointer the caller passed.
   That is enough to substitute the name, and not enough to treat the base as
   an incoming argument; `EntryAnchored` records the difference. */
void copy_after_reassign(char *buf, char *other)
{
    buf = other;
    char *p = buf;
    use(p); /* xj-expect: copy_after_reassign: p -> buf */
}

/* 12. The store reaches the use around the back edge, so `buf` may already
   have moved by the time `p` is used. */
void param_reassigned_in_loop(char *buf, char *other, int n)
{
    char *p = buf;
    for (int i = 0; i < n; i++)
    {
        use(p); /* xj-expect: param_reassigned_in_loop: p -> none */
        buf = other;
    }
}
