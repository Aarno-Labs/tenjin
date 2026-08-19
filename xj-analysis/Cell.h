// Cell — the alphabet of the must-equality domain.
//
// A cell is an abstract unit of storage, named by an access path
// `Root Step*`:
//
//     buf          Var(buf)
//     x.buf        Var(x) . Field(buf)
//     t->storage   Var(t) . Deref . Field(storage)
//     (supporting indices would require reasoning about arithmetic)
//
// Cells are interned; `CellId` is the identity used everywhere else, and
// nothing outside `CellUniverse` compares paths structurally.
//
// The cell set for a function is fixed by `collect` before the fixpoint
// starts and never grows.
#pragma once

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Sequence.h"

#include <optional>
#include <string>

namespace clang
{
  class ASTContext;
} // namespace clang

namespace xj::analysis
{

  // Interned identity of a cell. Ids are dense in [0, CellUniverse::size()),
  // so iteration over cells is a loop over indices.
  enum class CellId : unsigned
  {
  };
} // namespace xj::analysis

namespace llvm
{
  template <>
  struct enum_iteration_traits<xj::analysis::CellId>
  {
    static constexpr bool is_iterable = true;
  };
} // namespace llvm

namespace xj::analysis
{
  // One step of an access path.
  class Step
  {
  public:
    enum class Kind : unsigned char
    {
      Deref,
      Field
    };

    static Step deref();
    static Step field(const clang::FieldDecl *F);

    Kind kind() const { return K; }
    // Non-null iff `kind() == Kind::Field`.
    const clang::FieldDecl *field() const { return F; }

    friend bool operator==(Step A, Step B);
    friend bool operator!=(Step A, Step B) { return !(A == B); }

  private:
    Step(Kind K, const clang::FieldDecl *F) : K(K), F(F) {}

    Kind K;
    const clang::FieldDecl *F;
  };

  // An abstract memory cell. `Root` is a parameter, a local, or a global;
  // globals are cells (so a store through one is modelled) but are never
  // offered as resolution candidates.
  struct Cell
  {
    const clang::VarDecl *Root = nullptr;
    llvm::SmallVector<Step, 2> Path;

    friend bool operator==(const Cell &A, const Cell &B);
    friend bool operator!=(const Cell &A, const Cell &B) { return !(A == B); }
  };

  // The finite set of cells the analysis of one function may mention.
  class CellUniverse
  {
  public:
    // Access paths longer than this are not nameable, and expressions that
    // would need one evaluate to unknown.
    static constexpr unsigned MaxPathDepth = 2;

    // Every named, non-volatile variable whatever its
    // type, plus every `x.f` / `x->f` the body mentions, up to
    // `MaxPathDepth`. The non-pointer variables are not decoration; see the
    // note on `CellCollector` in `Cell.cpp`. A store whose destination is
    // not a cell havocs every cell not out of reach, and every
    // Deref-bearing cell is in that set, so an `i++` standing beside a
    // `t->storage` would clear it on every iteration.
    static CellUniverse collect(clang::ASTContext &Ctx,
                                const clang::FunctionDecl &FD);

    // The cell `E` names, or nullopt when this domain cannot name it:
    // array subscripts, call results, casts between unrelated types, paths
    // over the depth cap, anything not rooted at a `VarDecl`. A nullopt
    // here is what makes a store an *unresolvable* store.
    std::optional<CellId> lookup(const clang::Expr *E) const;
    std::optional<CellId> lookup(const clang::VarDecl *V) const;

    const Cell &get(CellId C) const;
    clang::QualType typeOf(CellId C) const;
    // The spelling a resolution reports, e.g. "t->storage".
    std::string print(CellId C) const;

    unsigned size() const;

    auto ids() const { return llvm::enum_seq(id(0), id(size())); }
    auto idsFrom(CellId From) const { return llvm::enum_seq(From, id(size())); }
    static CellId id(unsigned Index) { return static_cast<CellId>(Index); }
    static unsigned index(CellId C) { return static_cast<unsigned>(C); }
    static CellId next(CellId C) { return id(index(C) + 1); }

  private:
    CellUniverse();

    // Interns `C`, returning the existing id if the path is already known.
    CellId intern(const Cell &C);

    llvm::SmallVector<Cell, 16> Cells;
    // Fast path for the deref-free, field-free case, which is every cell at
    // M1 and most of them at M2.
    llvm::DenseMap<const clang::VarDecl *, unsigned> ByVar;
  };

} // namespace xj::analysis
