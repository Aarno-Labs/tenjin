/* The first thing to get working: M1, which is plain copy propagation.
 *
 * No structs appear anywhere in this file, so no `x.f` / `x->f` cell can
 * exist and the alphabet is variables whichever way `Options`
 * `MemoryCandidates` is set. That is the point — nothing here needs an
 * access path, a may-alias test, or any of §2's extensions beyond the
 * address-taken half of the out-of-reach rule.
 *
 * The functions are in implementation order. Each adds exactly one
 * mechanism to the one above it, so the file doubles as a checklist:
 * `copy_of_param` alone requires the cell universe, the entry store, T1's
 * CellRead arm, the worklist, and the all-sites fold, and is a reasonable
 * definition of "the analysis is alive".
 */

extern void use(char *);
extern char *get(void);
extern void g(void);
extern void take(char **);

/* 1. The whole pipeline, once. Cells {buf, p}; entry store binds each to
   Fresh(cell, entry()); T1 copies buf's symbol to p; one use site agrees.
   `buf` is a parameter that is never reassigned, so the shared symbol is
   Fresh(buf, entry()) and the resolution is additionally entry-anchored —
   which is what `detectRoots` needs and the only kind of base slicing can
   consume. */
void copy_of_param(char *buf)
{
    char *p = buf;
    use(p); /* xj-expect: copy_of_param: p -> buf */
}

/* 2. The fold. Two sites, both agreeing; the answer is their
   intersection, not their union. */
void two_uses(char *buf)
{
    char *p = buf;
    use(p);
    use(p); /* xj-expect: two_uses: p -> buf */
}

/* 3. The fold actually intersecting. Site one has {a}, site two has {b},
   and there is no single text that can be substituted at both. Getting
   this wrong — unioning instead of intersecting — produces an answer that
   looks plausible and is useless. */
void disagreeing_uses(char *a, char *b)
{
    char *p = a;
    use(p);
    p = b;
    use(p); /* xj-expect: disagreeing_uses: p -> none */
}

/* 4. T1's Opaque arm. A call result is not a value this domain models, so
   the destination is havoc'd rather than left holding its previous symbol.
   Leaving it is exactly the defect the M0 spike found in clang::dataflow
   (FINDINGS.md, F4), which produced 7 false must-equalities in 16 over the
   corpus, so this case is worth having early. */
void opaque_rhs(char *buf)
{
    char *p = buf;
    p = get();
    use(p); /* xj-expect: opaque_rhs: p -> none */
}

/* 5. The join. The branches disagree, so the lattice join yields T, and T
   is not an equivalence class: two cells at T are not equal to each
   other. */
void join_disagrees(char *a, char *b, int c)
{
    char *p;
    if (c)
        p = a;
    else
        p = b;
    use(p); /* xj-expect: join_disagrees: p -> none */
}

/* 6. The one clobber rule M1 exercises. Both cells are deref-free paths
   rooted at non-escaping locals — a C parameter is a local — so the callee
   cannot name either one and cannot write either one, under any aliasing.
   A call changes what a pointer parameter points *at*, never the parameter
   variable itself, and a named candidate does not depend on the pointee. */
void call_does_not_kill_a_local(char *buf)
{
    char *p = buf;
    g();
    use(p); /* xj-expect: call_does_not_kill_a_local: p -> buf */
}

/* 7. The other half of the same rule. `&buf` appears, so `buf` escapes and
   is no longer out of reach: `take` may have written through the pointer
   it was handed. `buf` is havoc'd at the call while `p` keeps its symbol,
   so they part company. Pairing this with 6 is the entire M1 soundness
   argument, and the two differ only in whether the base's address is
   taken. */
void address_taken_escapes(char *buf)
{
    char *p = buf;
    take(&buf);
    use(p); /* xj-expect: address_taken_escapes: p -> none */
}

/* 8. The address of the *cursor* rather than of the base, which is a
   different rule and not a clobber rule at all. `p`'s value agrees with
   `buf` at the only site there is, and the fact is still unusable: `&p`
   reads p's storage, not its value, so rewriting it to `&buf` would hand
   `take` the address of a different object — and `p` cannot be deleted
   while something still points at it.

   `UseCollector` already draws this line for the LHS of an assignment and
   for the operand of `++`. Excluding `&p` from the site set is not enough
   here, because then a lone `use(p)` elsewhere would carry the resolution
   and the pass would delete a variable whose address is taken. So the
   address-taken cursor is vetoed outright, and this reports `none` where
   every site agreed. */
void address_of_cursor(char *buf)
{
    char *p = buf;
    take(&p); /* xj-expect: address_of_cursor: p -> none */
}

/* 9. The base parameter itself is reassigned — the M1 counterpart of
   `field_paths.c`'s `reassign_base`, which reassigns the pointer a base
   path is *rooted at*. Here the base cell simply is the parameter, so the
   store detaches it directly rather than through the prefix rule, and the
   class `p` sits in keeps the `InitOf(buf)` label with no `buf` in it. The
   answer is `none` and nothing about the substitution needed to know: what
   makes it `none` is that a candidate has to agree at *every* site. */
void param_reassigned_before_use(char *buf, char *other)
{
    char *p = buf;
    buf = other;
    use(p); /* xj-expect: param_reassigned_before_use: p -> none */
}

/* 10. The same store, after the only site rather than before it, and the
   resolution stands. A parameter being reassigned *somewhere* is not a
   veto — the fold quantifies over use sites, and a store past the last one
   is not one. This is the half worth stating explicitly, because the
   conservative reading (a mutated base is unusable) is the one a reader
   expects and would be a real loss: substitution is textual at each site,
   and at every site here `p` and `buf` are the same value. */
void param_reassigned_after_use(char *buf, char *other)
{
    char *p = buf;
    use(p); /* xj-expect: param_reassigned_after_use: p -> buf */
    buf = other;
    use(buf);
}

/* 11. The copy taken after the store. `p`, `buf` and `other` share one
   class, so `buf` agrees and the resolution is `buf` — by the spelling
   tie-break, not because `buf` is what was copied.

   This is the case `EntryAnchored` exists to separate, and the only one in
   this file where it is false: `p` equals what `buf` holds *here*, not what
   the caller passed. Both facts license the substitution; only the stronger
   one licenses treating the base as the incoming argument. The annotation
   syntax names the cell alone, so what is pinned here is the resolution —
   `--dump` is where the anchoring shows. */
void copy_after_reassign(char *buf, char *other)
{
    buf = other;
    char *p = buf;
    use(p); /* xj-expect: copy_after_reassign: p -> buf */
}

/* 12. The store and the use in the same loop, so the back edge carries a
   state where `buf` has already moved. The join at the loop head is what
   answers, and it answers `none` — the reason a site-by-site reading of 10
   ("the store is textually after the use") is not the rule. */
void param_reassigned_in_loop(char *buf, char *other, int n)
{
    char *p = buf;
    for (int i = 0; i < n; i++)
    {
        use(p); /* xj-expect: param_reassigned_in_loop: p -> none */
        buf = other;
    }
}
