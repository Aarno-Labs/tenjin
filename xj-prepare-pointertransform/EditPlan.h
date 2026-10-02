// EditPlan.h — one owner for the extent of every rewrite, and the
// guarantee that the rewrites do not collide.
//
// The hazard this exists to remove: an access whose rewrite replaces a
// whole node — `*(p + n)` becoming `p[p_index_xj + n]` — carries the text
// of its sub-expressions along, and a sub-expression can itself contain a
// reference this pass has to rewrite. Emitting both edits puts one range
// inside the other; emitting only one silently loses the other, and a
// lost edit is not a formatting blemish but a miscompilation.
//
// Composing the rewrites is xj::EditForest's job (EditForest.h): each is
// rendered with the ones nested inside it already in place, only the
// outermost are handed to the Rewriter, and one that was never rendered is
// reported, so a rewrite that copies source text over a live reference is
// a bug that is named instead of wrong C. This file decides what each
// rewrite covers and what it says.
//
// The alternative to nesting is always available — a reference can always
// be rewritten as the value read `(p + p_index_xj)`, whose extent is the
// name alone — so a plan that cannot compose is a bug in the classifier
// rather than a limitation of the input.

#pragma once

#include "Common.h"
#include "EditForest.h"

#include "llvm/ADT/STLFunctionalExtras.h"

// What a pointer's whole access list decides about each of its rewrites.
// These are questions about the pointer and not about any one access, so
// they are answered once, by pointerFacts, and handed to whatever looks
// at an access.
struct PointerFacts {
    // The -1 sentinel can reach this pointer's index, so every site turning
    // the index back into a pointer has to map it back to a real null:
    // `base + -1` addresses one element before the region and is
    // emphatically not null.
    //
    // The sentinel arrives from exactly four places. Two are explicit
    // nulls; the third is the generated wrapper for an allowlisted
    // function, which returns -1 for "not found" — so a strchr-derived
    // pointer can be null without any NULL appearing in the source. The
    // fourth is a pairing that copies: `p = q` takes q's index, sentinel
    // and all. A future kind that can assign -1 to an index belongs in this
    // list.
    bool may_be_null = false;

    // The sentinel can sit over a region that is not null, which happens
    // only through an allowlisted-function wrapper, or a copy of a pointer
    // it happens to. The index is then the only witness, and any negative
    // index is read as null — one that got there by stepping back past
    // where its pointer was seated included, which such a pointer cannot
    // tell apart. Everywhere else the region is null exactly when the
    // pointer is, so the region is what gets tested: `if (p)` already reads
    // correctly, and a negative index is only ever a position.
    bool null_in_index = false;

    bool operator==(const PointerFacts &o) const {
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

// The node whose source range `access` replaces. This is the single owner
// of the edit extent: validation, the planner and the renderer all ask
// here, so they cannot disagree about what a rewrite covers.
//
// Null when the access produces no edit: `sizeof p` never reads the value,
// an unsplit initializer keeps its text with the index simply starting at
// zero, and a null test stays as written unless `facts.null_in_index`.
//
// This is the extent of an access taken by itself. The plan goes on to drop
// the rewrite of a reference that a pairing carries instead; see
// EditPlan::pairedRoot.
const Stmt *editedNode(const PointerAccess &access, const PointerFacts &facts);

// EditPlan — every access rewrite in the translation unit, composed by an
// xj::EditForest.
//
// The plan is also where two indices are paired. `q = p + 1` can become
// `q = p` with `q_index_xj = p_index_xj + 1` only if p has an index, and the
// plan is the first place that knows which pointers do. It decides once, in
// pairedRoot, and both ends follow: the owner's index names the root's, and
// the root's own rewrite is dropped so the reference stays bare.
class EditPlan {
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

    // The initializer for `ptr`'s index declaration — the same question an
    // Init answers, asked at the point of declaration: `int *q = p + 1`
    // declares `int q_index_xj = p_index_xj + 1`. Rendered rather than
    // assembled from source text: the offset terms move out of the
    // right-hand side to get here, and one of them can read through a
    // pointer being rewritten. Follows build().
    std::string indexDeclInit(const VarDecl *ptr,
                              const std::vector<PointerAccess> &accesses);

    // The whole guarantee, checked before a single character is written:
    // the planned edits nest, every one of them was either emitted as a
    // root or rendered into one, and no insertion lands inside a
    // replacement that would swallow it. Node ranges cannot partially
    // overlap or coincide, so a failure to nest reports an edit extent
    // that came from somewhere other than editedNode(). Follows
    // appendRootEdits(), and takes the complete edit list so the
    // declaration insertions are covered too.
    bool verify(const std::vector<Edit> &edits);

  private:
    // One access's rewrite, before its text has been rendered.
    struct PlannedEdit {
        const VarDecl *ptr = nullptr;
        const PointerAccess *access = nullptr;
        const Stmt *node = nullptr;
        xj::FileRange range;  // the text `node` spans
    };

    // The pointer value for `ptr`'s index, guarded only where the sentinel
    // can actually reach. A pointer that never holds null keeps the bare
    // `base + index` spelling, which is what the slice pass matches.
    std::string pointerValue(const VarDecl *ptr);

    // Note that `func_name`'s wrapper has to be defined before the function
    // containing `use`. The earliest such function in the file wins, so one
    // body serves every use in the TU and no use can precede it.
    // `func_name` is the library function — `strchr` — not the wrapper it
    // is spelled through.
    void needWrapper(const std::string &func_name, const Stmt *use);

    // Where the text of a kept sub-expression comes from, with the rewrites
    // nested in it rendered: the forest's handle for the edit being
    // rendered, or the forest itself for text rendered into an insertion
    // rather than into an edit (an index declaration's terms).
    using TextOf = llvm::function_ref<std::string(SourceRange)>;

    // The text of `term`, an operand spliced into an index sum after a `+`
    // or a `-`, parenthesized where it would otherwise regroup: `p[i & 3]`
    // is `p[p_index_xj + (i & 3)]`. Rendered through `text_of`.
    //
    // `as_subscript` says the sum is used as a subscript as it stands. An
    // unsigned term would make it an unsigned sum, and an index that has
    // stepped back past where its pointer was seated would wrap around
    // instead of going negative, so such a term is cast to its signed type.
    // A sum assigned to an index is converted back on the way in.
    std::string renderTerm(const OffsetTerm &term, TextOf text_of,
                           bool as_subscript);

    // The replacement text for edit `i`. The forest calls this with its
    // handle `in` for the edit, and every sub-expression the text keeps is
    // fetched through that handle, so that a rewrite nested in one comes
    // along.
    std::string render(size_t i, const xj::EditForest::NestedEditRenderer &in);

    // The text of `S` through `text_of`: its source with each rewrite
    // inside it rendered in place. Asked of the very node an edit replaces,
    // by that edit, it is the node's text with the rewrites nested in it.
    std::string renderWithin(const Stmt *S, TextOf text_of);

    // The rewritten pointer that `access` starts from, or null. `access` is
    // an Init, an Assign or an AssignFromAllowedFunc, and the pointer is the
    // one its root names.
    //
    // Non-null means the two indices are paired: the index `access` installs
    // names the root's index, and the root's own reference gets no rewrite.
    const VarDecl *pairedRoot(const PointerAccess &access) const;

    // True when the split of an Init or Assign is used, so the right-hand
    // side keeps only its root. A split that steps its root — `q = p++` —
    // moves the step into the root's index, so it is used only when the root
    // is paired. Otherwise the right-hand side is kept whole, where `p++`
    // still moves p itself, and the index starts at 0.
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
