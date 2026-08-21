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
// Node ranges nest or are disjoint — two of them never partially overlap —
// so the planned edits form a forest. Everything here follows from that:
//
//   - only the *roots* of the forest are handed to the Rewriter, so the
//     ranges actually applied are disjoint by construction;
//   - a root's replacement text is rendered by splicing its descendants'
//     rendered text into its own source text, so nothing is dropped;
//   - rendering records which descendants it consumed, and verify()
//     confirms every one of them was, so a rewrite that copies source text
//     over a live reference is a reported bug instead of wrong C.
//
// The alternative to splicing is always available — a reference can always
// be rewritten as the value read `(p + p_index_xj)`, whose extent is the
// name alone — so a plan that cannot compose is a bug in the classifier
// rather than a limitation of the input.

#pragma once

#include "Common.h"

// True when `access` must produce an edit. False for the kinds that
// legitimately produce none: a pairwise root is carried by its owner's
// assignment, `sizeof p` never reads the value, and an unsplit
// initializer keeps its text with the index simply starting at zero.
//
// A null test is not among them unconditionally — see nullLivesInIndex —
// so it is asked separately, through nullTestNode.
bool accessNeedsEdit(const PointerAccess &access);

// True when this pointer can carry a negative index while its region stays
// non-null, which happens only through an allowlisted-function wrapper. It
// is the one condition under which a null test has to be rewritten: a
// pointer whose only null is `p = NULL` reseats the region and really is
// null, so `if (p)` already reads correctly.
bool nullLivesInIndex(const std::vector<PointerAccess> &accesses);

// True when the -1 sentinel can reach this pointer's index at all, so that
// every site turning the index back into a pointer has to map it back to a
// real null. `base + -1` addresses one element before the region and is
// emphatically not null.
//
// The sentinel arrives from exactly three places. Two are explicit nulls;
// the third is the generated wrapper for an allowlisted function, which
// returns -1 for "not found" — so a strchr-derived pointer can be null
// without any NULL appearing in the source. A future kind that can assign
// -1 to an index belongs in this list.
bool pointerMayBeNull(const std::vector<PointerAccess> &accesses);

// The node a null-test rewrite replaces: the whole comparison for
// `p == NULL` / `p != NULL`, which compares against a pointer, and the
// reference itself for every other form — `if (p)`, `!p`, `p && q` — which
// are truth tests on the reference. Null when this access is not a null
// test. Asked only when nullLivesInIndex says the test has to move.
const Stmt *nullTestNode(const PointerAccess &access);

// The node whose source range `access` replaces, or null when it needs no
// edit (or, if it does need one, when the anchor cannot be found — which
// validation rejects). This is the single owner of the edit extent: the
// planner, the renderer and the verifier all ask here, so they cannot
// disagree about what a rewrite covers.
const Stmt *editedNode(const PointerAccess &access, ASTContext &Ctx);

// The file offsets `N` spans, or false when the range is unusable: inside
// a macro expansion, which the Rewriter cannot edit, or split across two
// files, whose offsets are not comparable.
bool editRangeOf(const Stmt *N, ASTContext &Ctx, FileID &file, unsigned &begin,
                 unsigned &end);

// EditPlan — every access rewrite in the translation unit, arranged as a
// nesting forest and rendered from the leaves up.
class EditPlan {
  public:
    EditPlan(ASTContext &Ctx, const std::set<const VarDecl *> &transformed);

    // Record one pointer's rewrites. Accesses that need no edit are
    // ignored. `accesses` must outlive the plan; it is read again during
    // rendering.
    //
    // The whole access list rather than one access at a time, because two
    // of the questions a rewrite asks are about the pointer and not about
    // the access: whether the sentinel can reach its index at all
    // (pointerMayBeNull) and whether a null test has to move
    // (nullLivesInIndex). `FD` is the function the pointer lives in, and
    // is where a generated wrapper body goes.
    void add(const FunctionDecl *FD, const VarDecl *ptr,
             const std::vector<PointerAccess> &accesses);

    // Arrange the recorded edits into the nesting forest. Node ranges
    // cannot partially overlap or coincide, so a hit reports an edit
    // extent that came from somewhere other than editedNode().
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
    // every planned edit was either emitted as a root or spliced into one,
    // the emitted replacements are pairwise disjoint, and no insertion
    // lands inside a replacement that would swallow it. Follows
    // appendRootEdits(), and takes the complete edit list so the
    // declaration insertions are covered too.
    bool verify(const std::vector<Edit> &edits);

  private:
    // One access's rewrite, before its text has been rendered.
    struct PlannedEdit {
        const FunctionDecl *FD = nullptr;  // where a wrapper body would go
        const VarDecl *ptr = nullptr;
        const PointerAccess *access = nullptr;
        const Stmt *node = nullptr;
        FileID file;
        unsigned begin = 0;
        unsigned end = 0;              // past-the-end offset
        std::vector<size_t> children;  // edits directly inside this one
        size_t parent = kNoParent;
        bool consumed = false;         // emitted as a root, or spliced into one
    };

    static constexpr size_t kNoParent = static_cast<size_t>(-1);

    // What a pointer's whole access list decided, kept where render() can
    // reach it from a single access.
    struct PointerFacts {
        bool may_be_null = false;
        bool null_in_index = false;
    };

    // The pointer value for `ptr`'s index, guarded only where the sentinel
    // can actually reach. A pointer that never holds null keeps the bare
    // `base + index` spelling, which is what the slice pass matches.
    std::string pointerValue(const VarDecl *ptr);

    // Note that `func_name`'s wrapper has to be defined before `FD`. The
    // earliest such function in the file wins, so one body serves every use
    // in the TU and no use can precede it. `func_name` is the library
    // function — `strchr` — not the wrapper it is spelled through.
    void needWrapper(const std::string &func_name, const FunctionDecl *FD);

    // The replacement text for edit `i`, with every descendant spliced in.
    std::string render(size_t i);

    // The source text of `S` — a sub-expression of edit `owner`'s node —
    // with each of `owner`'s children that lies inside `S` replaced by its
    // own rendered text.
    std::string renderWithin(const Stmt *S, size_t owner);

    // The index an Init or Assign installs. When the root is a pointer this
    // pass also rewrote, the two indices are paired — `p = q + 1` is
    // `p_index_xj = q_index_xj + 1`. Otherwise the root's value is still its
    // own position, so the offset counts from zero. `owner` is the edit the
    // terms were lifted out of, or kNoParent when there is none to render
    // against (an unsplit assignment, which has no terms).
    std::string renderIndexValue(const PointerAccess &access, size_t owner);

    // Report a broken invariant: a bug in this tool, so it is named loudly
    // and makes the run fail rather than quietly producing different C.
    void reportViolation(const llvm::Twine &what, SourceLocation loc);

    ASTContext &Ctx;
    const SourceManager &SM;
    const LangOptions &LO;
    const std::set<const VarDecl *> &transformed;
    std::vector<PlannedEdit> edits;
    std::map<const VarDecl *, PointerFacts> facts;
    // Library function name -> the earliest function needing its wrapper.
    // Emission is also gated on g_emitted_wrappers, so a body already
    // written for this file is not written twice.
    std::map<std::string, const FunctionDecl *> wrappers;
    // Violations are counted for the whole run, so a plan judges itself by
    // what it added rather than by the total.
    int violations_at_start = 0;
};
