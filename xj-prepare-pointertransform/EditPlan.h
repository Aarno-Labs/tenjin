// EditPlan.h — what each access rewrite covers and what it says.
//
// Rewrites nest: `*(p + n)` becomes `p[p_index_xj + n]`, and `n` can itself
// contain a reference this pass rewrites. Composing nested rewrites is
// xj::EditForest's job (EditForest.h). This file decides the extent of each
// rewrite (editedNode) and its text (EditPlan::render), taking every kept
// sub-expression through the forest so that the rewrites inside it come
// along.
//
// A reference can always be rewritten as the value read `(p + p_index_xj)`,
// whose extent is the name alone, so a plan the forest cannot compose is a
// bug in the classifier rather than a limitation of the input.

#pragma once

#include "Common.h"
#include "EditForest.h"

#include "llvm/ADT/STLFunctionalExtras.h"

// What a pointer's whole access list decides about each of its rewrites.
// These are questions about the pointer and not about any one access, so
// they are answered once, by pointerFacts, and handed to whatever looks
// at an access.
struct PointerFacts
{
    // The -1 sentinel can reach this pointer's index, so every site turning
    // the index back into a pointer has to map it back to a real null.
    bool may_be_null = false;

    // The index can be -1 while the base pointer is nonnull (i.e. via the strchr wrapper)
    bool null_in_index = false;

    bool operator==(const PointerFacts &o) const
    {
        return may_be_null == o.may_be_null && null_in_index == o.null_in_index;
    }
};

// The pointers being rewritten, each with its accesses.
using RewrittenPointers =
    std::map<const VarDecl *, const std::vector<PointerAccess> *>;

// The facts of every pointer in `rewritten`: what its own accesses decide,
// and what reaches it through a pairing that copies another's index.
// Nothing about a pointer that is not rewritten reaches anyone — its value
// arrives as a region.
std::map<const VarDecl *, PointerFacts>
pointerFacts(const RewrittenPointers &rewritten);

// The node whose source range `access` replaces
const Stmt *editedNode(const PointerAccess &access, const PointerFacts &facts);

// EditPlan — every access rewrite in the translation unit, composed by an
// xj::EditForest.
//
// The plan is also where two indices are paired. `q = p + 1` can become
// `q = p` with `q_index_xj = p_index_xj + 1`.
class EditPlan
{
public:
    // `facts` is pointerFacts() of the pointers in `transformed`.
    EditPlan(ASTContext &Ctx, const std::set<const VarDecl *> &transformed,
             const std::map<const VarDecl *, PointerFacts> &facts);

    // Record one pointer's rewrites. Accesses that need no edit are
    // ignored. `accesses` must outlive the plan; it is read again during
    // rendering.
    void add(const VarDecl *ptr, const std::vector<PointerAccess> &accesses);

    // Drop the rewrites that pairing replaces, and hand the rest to the
    // forest. Follows the last add().
    void build();

    // Render every root and append it to `edits`, followed by the body of
    // each `_index_xj` wrapper the rendering turned out to need. Follows
    // build().
    void appendRootEdits(std::vector<Edit> &edits);

    // The initializer for `ptr`'s index declaration
    std::string indexDeclInit(const VarDecl *ptr,
                              const std::vector<PointerAccess> &accesses);

    // Report the edits the forest would lose (EditForest::verify) as
    // violations, and check that no index declaration insertion lands
    // inside a replacement. False when this plan added a violation.
    bool verify(const std::vector<Edit> &edits);

private:
    // One access's rewrite, before its text has been rendered.
    struct PlannedEdit
    {
        const VarDecl *ptr = nullptr;
        const PointerAccess *access = nullptr;
        const Stmt *node = nullptr;
        xj::FileRange range; // the text `node` spans
    };

    // The expression rebuilding `ptr` from its base and index (possibly with a null guard)
    std::string pointerValue(const VarDecl *ptr);

    // Record that this TU needs the index wrapper for the library function
    // `func_name`, emitted ahead of the earliest function that uses it.
    void needWrapper(const std::string &func_name, const Stmt *use);

    using TextOf = llvm::function_ref<std::string(SourceRange)>;

    // The text of `term`, an operand spliced into an index sum after a `+`
    // or a `-`, parenthesized where it would otherwise regroup: `p[i & 3]`
    // is `p[p_index_xj + (i & 3)]`. Rendered through `text_of`.
    //
    // `as_subscript` says the sum is used as a subscript.
    std::string renderTerm(const OffsetTerm &term, TextOf text_of,
                           bool as_subscript);

    // The forest's RenderFn: the replacement text for edit `i`.
    std::string render(size_t i, const xj::EditForest::NestedEditRenderer &in);

    // The text of `S` through `text_of`, or "" for a null `S`.
    std::string renderWithin(const Stmt *S, TextOf text_of);

    // The variable named by the root of `access`, if that variable is itself
    // rewritten; null otherwise. With `p` rewritten and `buf` not:
    //     q = p + 1;      // p:    (q = p, q_index_xj = p_index_xj + 1)
    //     q = buf + 1;    // null: (q = buf, q_index_xj = 1)
    // In the first line `p` keeps its text instead of becoming
    // `(p + p_index_xj)`, since its position is already in q_index_xj.
    const VarDecl *pairedRoot(const PointerAccess &access) const;

    // True when the right-hand side of an Init or Assign is replaced by its
    // root, with the offset and any step moved into the index. With `p`
    // rewritten and `r` not:
    //     q = p + 1;      // true:  (q = p, q_index_xj = p_index_xj + 1)
    //     q = r + 1;      // true:  (q = r, q_index_xj = 1)
    //     q = p++;        // true:  (q = p, q_index_xj = p_index_xj++)
    //     q = r++;        // false: (q = r++, q_index_xj = 0)
    // The last is kept whole because only an index can carry the step, and
    // `r` has none.
    bool splitStands(const PointerAccess &access) const;

    // The index an Init or Assign installs. When the root is paired, the
    // index counts from the root's — `p = q + 1` is
    // `p_index_xj = q_index_xj + 1`. Otherwise the root's value is still its
    // own position, so the offset counts from zero. The terms are rendered
    // through `text_of`.
    std::string renderIndexValue(const PointerAccess &access, TextOf text_of);

    // Report a broken invariant: a bug in this tool, so it is named loudly
    // and makes the run fail rather than quietly producing different C.
    void reportViolation(const llvm::Twine &what, SourceLocation loc);

    ASTContext &Ctx;
    const SourceManager &SM;
    const LangOptions &LO;
    const std::set<const VarDecl *> &transformed;
    // Once build() has run, indexed by the forest's edit ids.
    std::vector<PlannedEdit> edits;
    xj::EditForest forest;
    // The root reference of every paired access. These are the references
    // whose own rewrite build() drops.
    std::set<const Expr *> paired_roots;
    // Kept per pointer so that render() can reach them from a single access.
    const std::map<const VarDecl *, PointerFacts> &facts;
    // Library function name -> the earliest function needing its wrapper.
    // There is one plan per translation unit, so one entry is one body per
    // file.
    std::map<std::string, const FunctionDecl *> wrappers;
    // Violations are counted for the whole run, so a plan judges itself by
    // what it added rather than by the total.
    int violations_at_start = 0;
};
