// SED — the must-equality domain: a partition of cells, with labels.
//
//     Label  =  InitOf(CellId) | AddrOf(CellId) | Const(APSInt)
//     State  =  _|_ | (P, L)     P a partition of Cells
//                                L a partial, injective map Class -> Label
//
// Two cells in one class provably hold the same value on every execution
// reaching the program point; every member of a labelled class provably
// holds that label's value.
//
// This is Gulwani and Necula's Strong Equivalence DAG (SAS 2004, §3)
// restricted to the case where there are no operation nodes: a node
// `<V, t>` is one class `V` plus its optional label `t`.

#pragma once

#include "Cell.h"

#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/SmallVector.h"

#include <optional>
#include <string>

namespace xj::analysis
{

  // The one value, nameable without reference to the partition itself,
  // that every member of a labelled class holds.
  class Label
  {
  public:
    enum class Kind : unsigned char
    {
      InitOf, // Denotes the initial value of a function parameter
      AddrOf,
      Const
    };

    // The value `C` held at function entry.
    static Label initOf(CellId C);
    // The address of `C`. Earns its place on array-to-pointer decay alone:
    // `char buf[64]; char *p = buf;`.
    static Label addrOf(CellId C);
    // An integer or null pointer constant.
    static Label constant(const llvm::APSInt &V);

    Kind kind() const { return K; }
    CellId cell() const
    {
      assert(kind() == Kind::InitOf || kind() == Kind::AddrOf);
      return C;
    }
    const llvm::APSInt &value() const
    {
      assert(kind() == Kind::Const);
      return V;
    }

    friend bool operator==(const Label &A, const Label &B);
    friend bool operator!=(const Label &A, const Label &B) { return !(A == B); }

    std::string print(const CellUniverse &Cells) const;

  private:
    Label(Kind K, CellId C, llvm::APSInt V)
        : K(K), C(C), V(std::move(V)) {}

    Kind K;
    CellId C{};     // InitOf, AddrOf
    llvm::APSInt V; // Const
  };

  class SED
  {
  public:
    // Which class an operation means.
    //
    // A class with members is named by its leader. A class with none
    // cannot be — there is no cell to name it after — so the two classes
    // an assignment can conjure out of thin air travel as a *description*
    // and are created by `move`: "the class carrying this label" when no
    // class carries it yet, and "a class of its own" for an opaque
    // right-hand side.
    class Class
    {
    public:
      bool isEmpty() const { return !Leader.has_value(); }

      const std::optional<Label> &label() const { return TheLabel; }

      // An empty class equals nothing, not even itself
      friend bool operator==(const Class &A, const Class &B)
      {
        return A.Leader && B.Leader && *A.Leader == *B.Leader;
      }
      friend bool operator!=(const Class &A, const Class &B)
      {
        return !(A == B);
      }

    private:
      friend class SED;

      Class(std::optional<CellId> Leader, std::optional<Label> L)
          : Leader(Leader), TheLabel(std::move(L)) {}

      std::optional<CellId> Leader;
      std::optional<Label> TheLabel;
    };

    static SED bottom(const CellUniverse &Cells);
    static SED atEntry(const CellUniverse &Cells);

    bool isBottom() const { return Bottom; }
    unsigned size() const { return Leader.size(); }
    // The label on `C`'s class, if any.
    std::optional<Label> labelOf(CellId C) const;

    // `A` and `B` are known equal.
    bool sameClass(CellId A, CellId B) const;

    // `C`'s class, in ascending cell order (`C` included).
    llvm::SmallVector<CellId, 4> membersOf(CellId C) const;

    // `C` leaves its class and becomes an unlabelled singleton.
    void detach(CellId C);

    void move(CellId C, Label L);
    void move(CellId C, CellId Dest);

    // `*this = *this join Other`; returns whether `*this` changed. This is
    // the worklist's change test, so it must be exact.
    bool join(const SED &Other);

    bool operator==(const SED &Other) const;
    bool operator!=(const SED &Other) const { return !(*this == Other); }

    // One class per line, e.g. "{p, q, t->storage} = &buf".
    std::string print(const CellUniverse &Cells) const;

  private:
    // `detach(C)`, then add `C` to `K` — creating `K` if it was an empty
    // class. Applied only where the statement makes `C` equal to `K`'s
    // members, or makes it compute `K`'s label.
    void move(CellId C, const Class &K);

    // A class with no members and no label
    // `move(c, newClass())` is exactly `detach(c)`.
    static Class newClass() { return Class(std::nullopt, std::nullopt); }

    // The class containing `C`.
    Class classOf(CellId C) const;

    // The class carrying `L`, or an empty class describing it when no class
    // does.
    Class classFor(const Label &L) const;

    explicit SED(const CellUniverse &Cells);

    // Whether `I` is the smallest cell index in its class.
    bool isLeader(CellId I) const { return Leader[CellUniverse::index(I)] == I; }
    // The smallest member of the class led by `L` other than `L` itself, if
    // the class has one.
    std::optional<CellId> nextMember(CellId L) const;
    const CellUniverse *Cells;
    // Cell index -> the smallest cell index in its class. Choosing the
    // smallest makes the representation canonical, so two states are equal
    // iff their arrays are equal and the worklist's change test needs no
    // normalization pass. `detach`, `move`, `sameClass` and `membersOf` are
    // O(|cells|) scans over a few dozen elements; a flat array beats
    // union-find here precisely because no class merge ever happens, and
    // because `join` and equality want a canonical dense form anyway.
    llvm::SmallVector<CellId, 16> Leader;
    // Indexed by leader. Held at `nullopt` for every non-leader index, so
    // that the array compare above stays a valid state equality.
    llvm::SmallVector<std::optional<Label>, 16> LabelOf;
    bool Bottom = true;
  };

} // namespace xj::analysis
