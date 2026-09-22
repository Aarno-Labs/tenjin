#include "TUState.h"

#include <algorithm>

using namespace clang;

namespace xj {

llvm::json::Array RunResults::diagnosticsJson() const {
  llvm::json::Array Out;
  for (const Diagnostic &D : Diagnostics)
    Out.push_back(llvm::json::Object{{"kind", D.Kind},
                                     {"site", D.Site},
                                     {"subject", D.Subject},
                                     {"message", D.Message}});
  return Out;
}

std::string XjType::rustText() const {
  if (!Key)
    return "";
  std::string Out;
  for (unsigned I = 0; I < AddrOf; ++I)
    Out += "*mut ";
  return Out + Key->Rust.str();
}

void LocalPlacement::placeTypedef(const TypedefKey &K, unsigned Offset) {
  auto [It, Inserted] = Typedefs.try_emplace(K.Id, Offset, &K);
  if (!Inserted)
    It->second.first = std::min(It->second.first, Offset);
}

void LocalPlacement::placeMarker(const MarkerKey &K, unsigned Offset) {
  auto [It, Inserted] = Markers.try_emplace(K.Id, Offset, &K);
  if (!Inserted)
    It->second.first = std::min(It->second.first, Offset);
}

static void emitTypedef(const TypedefKey &K, const Registry &R,
                        const std::set<std::string> &AtOffset,
                        std::set<std::string> &Done, std::string &Out) {
  if (!AtOffset.count(K.Id) || !Done.insert(K.Id).second)
    return;
  for (const std::string &Id : K.Decl.Typedefs)
    if (const TypedefKey *Inner = R.typedefById(Id))
      emitTypedef(*Inner, R, AtOffset, Done, Out);
  Out += R.definition(K) + "\n";
}

void LocalPlacement::emit(EditSet &Edits, const Registry &R) const {
  std::map<unsigned, std::set<std::string>> TypedefsAt;
  for (const auto &[Id, Placed] : Typedefs)
    TypedefsAt[Placed.first].insert(Id);
  std::map<unsigned, std::map<std::string, const MarkerKey *>> MarkersAt;
  for (const auto &[Id, Placed] : Markers)
    MarkersAt[Placed.first][R.name(*Placed.second)] = Placed.second;

  for (const auto &[Offset, Ids] : TypedefsAt) {
    std::string Text;
    std::set<std::string> Done;
    for (const std::string &Id : Ids)
      emitTypedef(*R.typedefById(Id), R, Ids, Done, Text);
    Edits.insertBefore(Offset, Text);
  }
  for (const auto &[Offset, ByName] : MarkersAt) {
    std::string Text;
    for (const auto &[_, K] : ByName)
      Text += R.definition(*K) + "\n";
    Edits.insertBefore(Offset, Text);
  }
}

TUState::TUState(ASTContext &Ctx, const Guidance &G, Registry &R,
                 RunResults *Results)
    : Ctx(Ctx), SM(Ctx.getSourceManager()), G(G), Reg(R), Results(Results),
      Speller(Ctx), Edits(Ctx.getSourceManager(), Ctx.getLangOpts()) {}

bool TUState::inMainFile(SourceLocation Loc) const {
  return Loc.isValid() && SM.isWrittenInMainFile(SM.getFileLoc(Loc));
}

std::string TUState::site(SourceLocation Loc) const {
  PresumedLoc P = SM.getPresumedLoc(SM.getFileLoc(Loc));
  if (P.isInvalid())
    return "<unknown>";
  return std::string(P.getFilename()) + ":" + std::to_string(P.getLine()) +
         ":" + std::to_string(P.getColumn());
}

void TUState::diagnose(llvm::StringRef Kind, SourceLocation Loc,
                       llvm::StringRef Subject, llvm::StringRef Message) {
  if (Results)
    Results->Diagnostics.push_back(
        {Kind.str(), site(Loc), Subject.str(), Message.str()});
}

void TUState::computeTopLevelRanges() {
  for (const Decl *D : Ctx.getTranslationUnitDecl()->decls())
    if (auto R = Edits.offsets(D->getSourceRange()))
      TopLevelRanges.push_back(*R);
  std::sort(TopLevelRanges.begin(), TopLevelRanges.end());
  // `typedef struct {...} T;` is a record and a typedef whose ranges
  // overlap; the declaration starts where the outer one does.
  std::vector<std::pair<unsigned, unsigned>> Merged;
  for (const auto &R : TopLevelRanges) {
    if (!Merged.empty() && R.first < Merged.back().second)
      Merged.back().second = std::max(Merged.back().second, R.second);
    else
      Merged.push_back(R);
  }
  TopLevelRanges = std::move(Merged);
}

unsigned TUState::topLevelOffset(SourceLocation Loc) {
  if (TopLevelRanges.empty())
    computeTopLevelRanges();
  unsigned Offset = SM.getFileOffset(SM.getFileLoc(Loc));
  auto It = std::upper_bound(
      TopLevelRanges.begin(), TopLevelRanges.end(), Offset,
      [](unsigned O, const std::pair<unsigned, unsigned> &R) {
        return O < R.first;
      });
  if (It != TopLevelRanges.begin() && std::prev(It)->second > Offset)
    return std::prev(It)->first;
  return Offset;
}

EditSet::Layer TUState::callLayer(const MarkerKey &K) const {
  const Registry *R = &Reg;
  const MarkerKey *M = &K;
  return [R, M](const std::string &Inner) {
    return R->name(*M) + "(" + Inner + ")";
  };
}

void TUState::placeLocal(const TypedefKey &K, SourceLocation Use) {
  if (Reg.isHeaderSafe(K))
    return;
  unsigned Offset = topLevelOffset(Use);
  Spelling Self;
  Self.Typedefs.insert(K.Id);
  for (const TypedefKey *T : Reg.typedefClosure(Self))
    if (!Reg.isHeaderSafe(*T))
      Local.placeTypedef(*T, Offset);
}

void TUState::placeLocal(const MarkerKey &K, SourceLocation Use) {
  if (Reg.isHeaderSafe(K))
    return;
  unsigned Offset = topLevelOffset(Use);
  for (const Spelling *S : {&K.Ret, &K.Param})
    for (const TypedefKey *T : Reg.typedefClosure(*S))
      if (!Reg.isHeaderSafe(*T))
        Local.placeTypedef(*T, Offset);
  Local.placeMarker(K, Offset);
}

} // namespace xj
