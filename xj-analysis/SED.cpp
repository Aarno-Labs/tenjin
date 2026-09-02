#include "SED.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"

#include <cassert>
#include <utility>

namespace xj::analysis
{

  Label Label::initOf(CellId C)
  {
    return Label(Kind::InitOf, C, llvm::APSInt());
  }

  Label Label::addrOf(CellId C)
  {
    return Label(Kind::AddrOf, C, llvm::APSInt());
  }

  Label Label::constant(const llvm::APSInt &V)
  {
    return Label(Kind::Const, CellId{}, V);
  }

  bool operator==(const Label &A, const Label &B)
  {
    if (A.K != B.K)
      return false;
    if (A.K == Label::Kind::Const)
      // Not `APSInt::operator==`, which requires equal bit widths.
      return llvm::APSInt::isSameValue(A.V, B.V);
    return A.C == B.C;
  }

  std::string Label::print(const CellUniverse &Cells) const
  {
    std::string Out;
    llvm::raw_string_ostream OS(Out);
    switch (K)
    {
    case Kind::InitOf:
      OS << "init(" << Cells.print(C) << ")";
      break;
    case Kind::AddrOf:
      OS << "&" << Cells.print(C);
      break;
    case Kind::Const:
      OS << V;
      break;
    }
    return OS.str();
  }

  SED::SED(const CellUniverse &U) : Cells(&U), Leader(U.size()), LabelOf(U.size())
  {
    // Every cell alone in its own class, so every cell leads.
    for (CellId C : Cells->ids())
      Leader[CellUniverse::index(C)] = C;
  }

  SED SED::bottom(const CellUniverse &Cells)
  {
    SED S(Cells);
    S.Bottom = true;
    return S;
  }

  SED SED::atEntry(const CellUniverse &Cells)
  {
    SED S(Cells);
    S.Bottom = false;
    for (CellId C : S.Cells->ids())
      S.LabelOf[CellUniverse::index(C)] = Label::initOf(C);
    return S;
  }

  std::optional<CellId> SED::nextMember(CellId L) const
  {
    // A leader is the smallest index in its class, so the rest of the class
    // lies above it.
    for (CellId J : Cells->idsFrom(CellUniverse::next(L)))
    {
      if (Leader[CellUniverse::index(J)] == L)
        return J;
    }
    return std::nullopt;
  }

  SED::Class SED::classOf(CellId C) const
  {
    assert(!Bottom && "classOf on an unreached block");
    CellId L = Leader[CellUniverse::index(C)];
    return Class(L, LabelOf[CellUniverse::index(L)]);
  }

  SED::Class SED::classFor(const Label &L) const
  {
    assert(!Bottom && "classFor on an unreached block");
    // At most one class carries a given label, so the first hit is the only
    // one.
    for (CellId C : Cells->ids())
    {
      auto I = CellUniverse::index(C);
      if (isLeader(C) && LabelOf[I] && *LabelOf[I] == L)
        return Class(C, L);
    }
    return Class(std::nullopt, L);
  }

  std::optional<Label> SED::labelOf(CellId C) const
  {
    if (Bottom)
      return std::nullopt;
    return LabelOf[CellUniverse::index(Leader[CellUniverse::index(C)])];
  }

  bool SED::sameClass(CellId A, CellId B) const
  {
    if (Bottom)
      return false;
    return Leader[CellUniverse::index(A)] == Leader[CellUniverse::index(B)];
  }

  llvm::SmallVector<CellId, 4> SED::membersOf(CellId C) const
  {
    llvm::SmallVector<CellId, 4> Members;
    if (Bottom)
      return Members;
    for (CellId C : Cells->idsFrom(C))
      if (Leader[CellUniverse::index(C)] == C)
        Members.push_back(C);
    return Members;
  }

  void SED::detach(CellId C)
  {
    // No execution reaches an unreached block, so there is nothing to
    // forget there.
    if (Bottom)
      return;

    unsigned I = CellUniverse::index(C);
    unsigned Old = CellUniverse::index(Leader[I]);
    std::optional<Label> OldLabel = LabelOf[Old];

    Leader[I] = C;
    LabelOf[I] = std::nullopt;

    if (Old != I)
      return; // `C` did not lead; the class it left is otherwise unchanged.

    // `C` led its class, so leadership passes to the next member, and the
    // label with it. If there is no next member the class is gone, and its
    // label is gone too — which for `InitOf` is exactly right: once nothing
    // holds a cell's incoming value, that value is no longer nameable.
    std::optional<CellId> Next = nextMember(C);
    if (!Next)
      return;

    // for (unsigned J = *Next, N = Leader.size(); J != N; ++J)
    for (CellId J : Cells->idsFrom(*Next))
    {
      if (Leader[CellUniverse::index(J)] == C)
        Leader[CellUniverse::index(J)] = *Next;
    }
    LabelOf[CellUniverse::index(*Next)] = std::move(OldLabel);
  }
  void SED::move(CellId C, Label L)
  {
    move(C, classFor(L));
  }

  void SED::move(CellId C, CellId Dest)
  {
    move(C, classOf(Dest));
  }

  void SED::move(CellId C, const Class &K)
  {
    assert(!Bottom && "move on an unreached block");

    // The paper's line-5 guard, read as node identity rather than type
    // equality: skip the move only when `C` is already in `K`. An empty
    // class compares equal to nothing, so this never skips a creation.
    if (K == classOf(C))
      return;

    // Read `K` out before detaching, since the detach may change which cell
    // leads the class `C` is leaving.
    std::optional<CellId> Target = K.Leader;
    std::optional<Label> Wanted = K.TheLabel;

    detach(C);
    unsigned I = CellUniverse::index(C);

    if (!Target)
    {
      // An empty class: the detach has already made `C` an unlabelled
      // singleton, so all that is left is the label the class was described
      // by, if any.
      assert((!Wanted || classFor(*Wanted).isEmpty()) &&
             "some class already carries this label");
      LabelOf[I] = std::move(Wanted);
      return;
    }

    CellId L = *Target;
    assert(L != C && "the guard above should have skipped this move");
    if (C > L)
    {
      Leader[I] = L;
      return;
    }

    // `C` is now the smallest index in the class, so it takes over as
    // leader — the label moves with the leadership, not with the cell.
    for (CellId J : Cells->ids())
      if (Leader[CellUniverse::index(J)] == L)
        Leader[CellUniverse::index(J)] = C;
    LabelOf[I] = std::move(LabelOf[CellUniverse::index(L)]);
    LabelOf[CellUniverse::index(L)] = std::nullopt;
  }

  bool SED::join(const SED &Other)
  {
    if (Other.Bottom)
      return false;
    if (Bottom)
    {
      Leader = Other.Leader;
      LabelOf = Other.LabelOf;
      Bottom = false;
      return true;
    }
    assert(Leader.size() == Other.Leader.size() &&
           "joining states over different cell universes");

    // The product construction, in one pass: two cells land in one class of
    // the result iff they were in one class on *both* sides, so keying each
    // cell by the pair of leaders it has here and there is the whole join.
    // Visiting cells in index order makes the first cell carrying a key its
    // leader, which keeps the result canonical without sorting.
    const unsigned N = Leader.size();
    llvm::SmallVector<CellId, 16> NewLeader(N);
    llvm::SmallVector<std::optional<Label>, 16> NewLabelOf(N);
    llvm::DenseMap<std::pair<CellId, CellId>, CellId> LeaderForKey;

    for (CellId C : Cells->ids())
    {
      auto I = CellUniverse::index(C);
      auto Key = std::make_pair(Leader[I], Other.Leader[I]);
      CellId NewL = LeaderForKey.insert({Key, C}).first->second;
      NewLeader[I] = NewL;
      if (NewL != C)
        continue;

      // A label survives only where both sides' classes carried it. Each
      // side had at most one class carrying it, so the result does too.
      const std::optional<Label> &Mine = LabelOf[CellUniverse::index(Leader[I])];
      const std::optional<Label> &Theirs = Other.LabelOf[CellUniverse::index(Other.Leader[I])];
      if (Mine && Theirs && *Mine == *Theirs)
        NewLabelOf[I] = Mine;
    }

    // Both arrays are canonical, so this compare is exactly "the state
    // changed" — no normalization pass needed.
    bool Changed = NewLeader != Leader || NewLabelOf != LabelOf;
    Leader = std::move(NewLeader);
    LabelOf = std::move(NewLabelOf);
    return Changed;
  }

  bool SED::operator==(const SED &Other) const
  {
    if (Bottom != Other.Bottom)
      return false;
    if (Bottom)
      return true;
    return Leader == Other.Leader && LabelOf == Other.LabelOf;
  }

  std::string SED::print(const CellUniverse &Cells) const
  {
    std::string Out;
    llvm::raw_string_ostream OS(Out);
    if (Bottom)
    {
      OS << "_|_";
      return OS.str();
    }

    const char *Sep = "";
    for (CellId C : Cells.ids())
    {
      auto I = CellUniverse::index(C);
      if (!isLeader(C))
        continue;
      OS << Sep << "{";
      const char *MemberSep = "";
      for (CellId M : membersOf(C))
      {
        OS << MemberSep << Cells.print(M);
        MemberSep = ", ";
      }
      OS << "}";
      if (LabelOf[I])
        OS << " = " << LabelOf[I]->print(Cells);
      Sep = "\n";
    }
    return OS.str();
  }

} // namespace xj::analysis
