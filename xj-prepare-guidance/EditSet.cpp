#include "EditSet.h"

#include "clang/Lex/Lexer.h"

#include <climits>

using namespace clang;

namespace xj {

std::optional<std::pair<unsigned, unsigned>>
EditSet::offsets(SourceRange R) const {
  CharSourceRange File =
      Lexer::makeFileCharRange(CharSourceRange::getTokenRange(R), SM, LO);
  if (File.isInvalid())
    return std::nullopt;
  FileID Main = SM.getMainFileID();
  if (SM.getFileID(File.getBegin()) != Main ||
      SM.getFileID(File.getEnd()) != Main)
    return std::nullopt;
  return std::make_pair(SM.getFileOffset(File.getBegin()),
                        SM.getFileOffset(File.getEnd()));
}

bool EditSet::crossesExisting(Range R) const {
  for (const auto &[N, _] : Nodes) {
    bool Nested = (N.first <= R.first && R.second <= N.second) ||
                  (R.first <= N.first && N.second <= R.second);
    bool Disjoint = N.second <= R.first || R.second <= N.first;
    if (!Nested && !Disjoint)
      return true;
  }
  return false;
}

bool EditSet::addLayer(SourceRange SR, Layer L) {
  auto R = offsets(SR);
  if (!R || crossesExisting(*R))
    return false;
  Nodes[*R].push_back(std::move(L));
  return true;
}

void EditSet::insertBefore(unsigned Offset, std::string Text) {
  Insertions[Offset] += Text;
}

bool EditSet::hasEdits(const Stmt *S) const {
  auto R = offsets(S->getSourceRange());
  if (!R)
    return false;
  for (const auto &[N, _] : Nodes)
    if (R->first <= N.first && N.second <= R->second)
      return true;
  return false;
}

std::string EditSet::slice(unsigned Begin, unsigned End) const {
  return SM.getBufferData(SM.getMainFileID()).slice(Begin, End).str();
}

std::string EditSet::render(const Stmt *S) const {
  auto R = offsets(S->getSourceRange());
  if (!R)
    return "";
  return Nodes.count(*R) ? renderNode(*R) : renderChildren(*R, false);
}

std::string EditSet::renderNode(Range R) const {
  std::string Text = renderChildren(R, false);
  for (const Layer &L : Nodes.at(R))
    Text = L(Text);
  return Text;
}

std::string EditSet::renderChildren(Range R, bool TopLevel) const {
  std::string Out;
  unsigned Cursor = R.first;
  auto Ins = Insertions.lower_bound(R.first);
  auto emitInsertionsBefore = [&](unsigned Offset) {
    for (; TopLevel && Ins != Insertions.end() && Ins->first <= Offset; ++Ins) {
      if (Ins->first < Cursor)
        continue;
      Out += slice(Cursor, Ins->first) + Ins->second;
      Cursor = Ins->first;
    }
  };
  for (auto It = Nodes.lower_bound({R.first, UINT_MAX});
       It != Nodes.end() && It->first.first < R.second; ++It) {
    Range N = It->first;
    if (N == R || N.second > R.second || N.first < Cursor)
      continue;
    emitInsertionsBefore(N.first);
    Out += slice(Cursor, N.first) + renderNode(N);
    Cursor = N.second;
  }
  emitInsertionsBefore(R.second);
  return Out + slice(Cursor, R.second);
}

std::string EditSet::renderFile() const {
  unsigned Size = SM.getBufferData(SM.getMainFileID()).size();
  return renderChildren({0, Size}, true);
}

} // namespace xj
