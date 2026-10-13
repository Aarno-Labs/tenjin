#include "GuidanceEdits.h"

#include "clang/Lex/Lexer.h"

using namespace clang;

namespace xj {

Edit Edit::wrapIn(const MarkerKey &M, std::string Extra) {
  Edit E{Kind::Call};
  E.Marker = &M;
  E.IndexText = std::move(Extra);
  return E;
}

Edit Edit::call(const MarkerKey &M, const Expr *Base, const Expr *Index,
                std::string NoIndex) {
  Edit E{Kind::Call};
  E.Marker = &M;
  E.Base = Base;
  E.Index = Index;
  E.IndexText = std::move(NoIndex);
  return E;
}

Edit Edit::subscript(const Expr *Base, const Expr *Index) {
  Edit E{Kind::Subscript};
  E.Base = Base;
  E.Index = Index;
  E.IndexText = "0";
  return E;
}

Edit Edit::deref(const Expr *Base) {
  Edit E{Kind::Deref};
  E.Base = Base;
  return E;
}

Edit Edit::keep(const Expr *Base) {
  Edit E{Kind::Keep};
  E.Base = Base;
  return E;
}

Edit Edit::advance(const MarkerKey &M, const Expr *Base, const Expr *Step) {
  Edit E{Kind::Advance};
  E.Marker = &M;
  E.Base = Base;
  E.Index = Step;
  E.IndexText = "1";
  return E;
}

Edit Edit::retype(const TypedefKey &K, std::string Before, std::string After) {
  Edit E{Kind::Retype};
  E.Typedef = &K;
  E.Before = std::move(Before);
  E.After = std::move(After);
  return E;
}

GuidanceEdits::GuidanceEdits(const SourceManager &SM, const LangOptions &LO,
                             const Registry &Reg)
    : SM(SM), LO(LO), Reg(Reg) {}

std::optional<FileRange> GuidanceEdits::mainFileRange(SourceRange R) const {
  auto File = fileRangeOf(R, SM, LO);
  if (!File || File->File != SM.getMainFileID())
    return std::nullopt;
  return File;
}

bool GuidanceEdits::crossesExisting(const FileRange &R) const {
  for (const auto &[N, _] : Planned) {
    bool Nested = (N.first <= R.Begin && R.End <= N.second) ||
                  (R.Begin <= N.first && N.second <= R.End);
    bool Disjoint = N.second <= R.Begin || R.End <= N.first;
    if (!Nested && !Disjoint)
      return true;
  }
  return false;
}

GuidanceEdits::Result GuidanceEdits::add(SourceRange SR, Edit E) {
  auto R = mainFileRange(SR);
  if (!R)
    return Result::OutsideMainFile;
  if (crossesExisting(*R))
    return Result::CrossesEdit;
  auto [It, New] = Planned.try_emplace({R->Begin, R->End}, Layers{SR, {}});
  if (!New && !E.wraps())
    return Result::DiscardsEdit;
  It->second.Edits.push_back(std::move(E));
  return Result::Made;
}

void GuidanceEdits::discard(SourceRange SR) {
  if (auto R = mainFileRange(SR))
    Planned.erase({R->Begin, R->End});
}

void GuidanceEdits::insertBefore(unsigned Offset, std::string Text) {
  Insertions[Offset] += Text;
}

llvm::StringRef GuidanceEdits::describe(Result R) {
  switch (R) {
  case Result::Made:
    return "";
  case Result::OutsideMainFile:
    return "the expression is not written contiguously in the file";
  case Result::CrossesEdit:
    return "the rewrite overlaps a rewrite already made";
  case Result::DiscardsEdit:
    return "the rewrite would discard a rewrite already made";
  }
  llvm_unreachable("unknown edit result");
}

// Text of `E`, rendered as `Text`, usable as the operand of a postfix
// operator.
static std::string postfixOperand(const Expr *E, std::string Text) {
  if (isa<DeclRefExpr, MemberExpr, ArraySubscriptExpr, CallExpr, ParenExpr>(
          E->IgnoreImpCasts()))
    return Text;
  return "(" + Text + ")";
}

std::string GuidanceEdits::render(const Layers &L, const Renderer &In) const {
  std::string Text = L.Edits.front().wraps() ? In.text(L.Range) : "";
  for (const Edit &E : L.Edits)
    Text = render(E, Text, In);
  return Text;
}

std::string GuidanceEdits::render(const Edit &E, const std::string &Below,
                                  const Renderer &In) const {
  std::string Base = E.Base ? In.text(E.Base->getSourceRange()) : Below;
  std::string Index =
      E.Index ? In.text(E.Index->getSourceRange()) : E.IndexText;
  switch (E.K) {
  case Edit::Kind::Call: {
    std::string Call = Reg.name(*E.Marker) + "(" + Base +
                       (Index.empty() ? "" : ", " + Index) + ")";
    switch (E.Wrapper) {
    case Edit::Around::None:
      return Call;
    case Edit::Around::Deref:
      return "(*" + Call + ")";
    case Edit::Around::Not:
      return "(!" + Call + ")";
    }
    llvm_unreachable("unknown wrapper");
  }
  case Edit::Kind::Subscript:
    return postfixOperand(E.Base, Base) + "[" + Index + "]";
  case Edit::Kind::Deref:
    return "(*" + Base + ")";
  case Edit::Kind::Keep:
    return Base;
  case Edit::Kind::Advance:
    return Base + " = " + Reg.name(*E.Marker) + "(" + Base + ", " + Index + ")";
  case Edit::Kind::Retype:
    return E.Before + Reg.resolve(typedefRef(E.Typedef->Id)) + E.After;
  }
  llvm_unreachable("unknown edit kind");
}

std::vector<GuidanceEdits::Lost> GuidanceEdits::apply(Rewriter &RW) const {
  // Indexed by the forest's edit ids.
  std::vector<const Layers *> Ids;
  EditForest Forest(SM, LO, [&](EditForest::EditId Id, const Renderer &In) {
    return render(*Ids[Id], In);
  });
  FileID Main = SM.getMainFileID();
  for (const auto &[R, L] : Planned) {
    Forest.add({Main, R.first, R.second});
    Ids.push_back(&L);
  }
  std::vector<EditForest::Replacement> Roots = Forest.renderRoots();
  std::vector<Lost> Problems;
  for (const EditForest::Problem &P : Forest.verify()) {
    const Layers &L = *Ids[P.Edit];
    Problems.push_back(
        {L.Range.getBegin(),
         Lexer::getSourceText(CharSourceRange::getTokenRange(L.Range), SM, LO)
             .str(),
         P.What});
  }
  if (!Problems.empty())
    return Problems;
  for (const EditForest::Replacement &Root : Roots)
    RW.ReplaceText(Root.Range.begin(SM), Root.Range.size(), Root.Text);
  // The Rewriter puts an insertion after earlier ones at its offset, but
  // before a replacement that starts there.
  SourceLocation Start = SM.getLocForStartOfFile(Main);
  for (const auto &[Offset, Text] : Insertions)
    RW.InsertText(Start.getLocWithOffset(Offset), Text);
  return {};
}

} // namespace xj
