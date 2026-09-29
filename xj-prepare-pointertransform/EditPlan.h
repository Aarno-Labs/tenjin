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

// What a pointer's whole access list decides about each of its rewrites.
// These are questions about the pointer and not about any one access, so
// they are answered once, by pointerFactsOf, and handed to whatever looks
// at an access.
struct PointerFacts {
    // The -1 sentinel can reach this pointer's index, so every site turning
    // the index back into a pointer has to map it back to a real null:
    // `base + -1` addresses one element before the region and is
    // emphatically not null.
    //
    // The sentinel arrives from exactly three places. Two are explicit
    // nulls; the third is the generated wrapper for an allowlisted
    // function, which returns -1 for "not found" — so a strchr-derived
    // pointer can be null without any NULL appearing in the source. A
    // future kind that can assign -1 to an index belongs in this list.
    bool may_be_null = false;

    // The index can go negative while the region stays non-null, which
    // happens only through an allowlisted-function wrapper. It is the one
    // condition under which a null test has to be rewritten: a pointer
    // whose only null is `p = NULL` reseats the region and really is null,
    // so `if (p)` already reads correctly.
    bool null_in_index = false;
};

PointerFacts pointerFactsOf(const std::vector<PointerAccess> &accesses);

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

// The file offsets `N` spans, or false when the range is unusable: inside
// a macro expansion, which the Rewriter cannot edit, or split across two
// files, whose offsets are not comparable.
bool editRangeOf(const Stmt *N, ASTContext &Ctx, FileID &file, unsigned &begin,
                 unsigned &end);

// EditPlan — every access rewrite in the translation unit, arranged as a
// nesting forest and rendered from the leaves up.
//
// The plan is also where two indices are paired. `q = p + 1` can become
// `q = p` with `q_index_xj = p_index_xj + 1` only if p has an index, and the
// plan is the first place that knows which pointers do. It decides once, in
// pairedRoot, and both ends follow: the owner's index names the root's, and
// the root's own rewrite is dropped so the reference stays bare.
class EditPlan {
  public:
    EditPlan(ASTContext &Ctx, const std::set<const VarDecl *> &transformed);

    // Record one pointer's rewrites. Accesses that need no edit are
    // ignored. `accesses` must outlive the plan; it is read again during
    // rendering.
    //
    // The whole access list rather than one access at a time, because some
    // of what a rewrite asks is about the pointer and not about the access;
    // see PointerFacts. `FD` is the function the pointer lives in, and is
    // where a generated wrapper body goes.
    void add(const FunctionDecl *FD, const VarDecl *ptr,
             const std::vector<PointerAccess> &accesses);

    // Drop the rewrites that pairing replaces, then arrange the rest into
    // the nesting forest. Node ranges cannot partially overlap or coincide,
    // so a hit reports an edit extent that came from somewhere other than
    // editedNode(). Follows the last add().
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
    // The root reference of every paired access. These are the references
    // whose own rewrite build() drops.
    std::set<const Expr *> paired_roots;
    // Kept per pointer so that render() can reach them from a single access.
    std::map<const VarDecl *, PointerFacts> facts;
    // Library function name -> the earliest function needing its wrapper.
    // There is one plan per translation unit, so one entry is one body per
    // file.
    std::map<std::string, const FunctionDecl *> wrappers;
    // Violations are counted for the whole run, so a plan judges itself by
    // what it added rather than by the total.
    int violations_at_start = 0;
};
