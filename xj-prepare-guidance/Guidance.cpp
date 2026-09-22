#include "Guidance.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>

using llvm::StringRef;

namespace xj {

bool isDerivedName(StringRef Spec, StringRef Instance) {
  return Spec == Instance || Spec == Instance.split(UniqueSuffix).first;
}

std::optional<DeclSpec> DeclSpec::parse(StringRef S) {
  if (S.empty() || S.count(':') > 1 || S.count('#') > 1 || S.count('@') > 1)
    return std::nullopt;
  DeclSpec D;
  D.Text = S.str();
  // The parser this replaces reads `file@fn:var#line`, whatever its comment
  // said; specifiers in the wild are written that way.
  if (S.contains('@')) {
    auto [File, Rest] = S.split('@');
    D.File = File.str();
    S = Rest;
  }
  if (S.contains('#')) {
    auto [Head, LineText] = S.split('#');
    if (LineText.getAsInteger(10, D.Line))
      D.Line = 0;
    S = Head;
  }
  auto [Fn, Var] = S.split(':');
  D.Fn = Fn.str();
  D.Var = Var.str();
  if (D.Fn.empty())
    return std::nullopt;
  return D;
}

unsigned DeclSpec::specificity() const {
  return (Fn != "*" ? 4 : 0) + (Line ? 2 : 0) + (File.empty() ? 0 : 1);
}

static bool locationMatches(const DeclSpec &Spec, const DeclFacts &D) {
  if (Spec.Line && Spec.Line != D.Line)
    return false;
  return Spec.File.empty() || D.File.empty() ||
         StringRef(D.File).ends_with(Spec.File);
}

static bool scopeMatches(const DeclSpec &Spec, const DeclFacts &D) {
  if (Spec.Fn == "*")
    return true;
  if (D.IsField)
    return isDerivedName(Spec.Fn, D.Record);
  if (!D.ParentFn.empty())
    return isDerivedName(Spec.Fn, D.ParentFn);
  return D.IsFileScope;
}

static bool nameMatches(const DeclSpec &Spec, const DeclFacts &D) {
  if (D.IsField)
    return Spec.Var == "*" || Spec.Var == D.Name;
  // A file-scope variable is named by the function slot: `g` or `g:*`.
  if (D.IsFileScope)
    return isDerivedName(Spec.Fn, D.Name);
  return Spec.Var == "*" || isDerivedName(Spec.Var, D.Name);
}

bool matches(const DeclSpec &Spec, const DeclFacts &D) {
  return locationMatches(Spec, D) && scopeMatches(Spec, D) &&
         nameMatches(Spec, D);
}

template <typename V>
static const Rule<V> *bestMatch(const std::vector<Rule<V>> &Rules,
                                const DeclFacts &D) {
  const Rule<V> *Best = nullptr;
  for (const Rule<V> &R : Rules) {
    if (!matches(R.Spec, D))
      continue;
    if (!Best || R.Spec.specificity() > Best->Spec.specificity() ||
        (R.Spec.specificity() == Best->Spec.specificity() &&
         R.Order < Best->Order))
      Best = &R;
  }
  return Best;
}

const Rule<RustType> *
Guidance::matchType(const DeclFacts &D, const Rule<RustType> **Conflict) const {
  const Rule<RustType> *Best = bestMatch(VarTypes, D);
  if (!Best)
    return nullptr;
  for (const Rule<RustType> &R : VarTypes)
    if (&R != Best && R.Spec.specificity() == Best->Spec.specificity() &&
        R.Value.str() != Best->Value.str() && matches(R.Spec, D))
      *Conflict = &R;
  return Best;
}

const Rule<bool> *Guidance::matchMut(const DeclFacts &D) const {
  return bestMatch(VarMut, D);
}

const std::pair<const std::string, RustType> *
Guidance::returnType(StringRef FnName) const {
  for (const auto &Entry : FnReturnTypes)
    if (isDerivedName(Entry.first, FnName))
      return &Entry;
  return nullptr;
}

bool Guidance::alreadyInstrumented() const {
  return Raw.get("marker_typedefs") != nullptr;
}

bool Guidance::isEmpty() const {
  return VarTypes.empty() && FnReturnTypes.empty() && VarMut.empty();
}

static std::vector<std::string> sortedKeys(const llvm::json::Object *O) {
  std::vector<std::string> Keys;
  if (O)
    for (const auto &KV : *O)
      Keys.push_back(KV.first.str());
  std::sort(Keys.begin(), Keys.end());
  return Keys;
}

static std::vector<std::string> specTexts(const llvm::json::Value &V) {
  if (auto S = V.getAsString())
    return {S->str()};
  std::vector<std::string> Out;
  if (const auto *A = V.getAsArray())
    for (const auto &E : *A)
      if (auto S = E.getAsString())
        Out.push_back(S->str());
  return Out;
}

static void loadVarTypes(Guidance &G) {
  const llvm::json::Object *O = G.Raw.getObject("vars_of_type");
  unsigned Order = 0;
  for (const std::string &TypeText : sortedKeys(O)) {
    RustType Ty = RustType::parse(TypeText);
    for (const std::string &Text : specTexts(*O->get(TypeText)))
      if (auto Spec = DeclSpec::parse(Text))
        G.VarTypes.push_back({*Spec, Ty, Order++});
  }
}

static void loadFnReturnTypes(Guidance &G) {
  const llvm::json::Object *O = G.Raw.getObject("fn_return_type");
  for (const std::string &Fn : sortedKeys(O))
    if (auto S = O->get(Fn)->getAsString())
      G.FnReturnTypes.emplace(Fn, RustType::parse(*S));
}

static void loadVarMut(Guidance &G) {
  const llvm::json::Object *O = G.Raw.getObject("vars_mut");
  unsigned Order = 0;
  for (const std::string &Text : sortedKeys(O)) {
    auto Spec = DeclSpec::parse(Text);
    auto IsMut = O->get(Text)->getAsBoolean();
    if (Spec && IsMut)
      G.VarMut.push_back({*Spec, *IsMut, Order++});
  }
}

llvm::Expected<Guidance> Guidance::load(StringRef Path) {
  auto Buf = llvm::MemoryBuffer::getFile(Path);
  if (!Buf)
    return llvm::createStringError(Buf.getError(), "cannot read %s",
                                   Path.str().c_str());
  auto Parsed = llvm::json::parse((*Buf)->getBuffer());
  if (!Parsed)
    return Parsed.takeError();
  Guidance G;
  if (auto *O = Parsed->getAsObject())
    G.Raw = std::move(*O);
  loadVarTypes(G);
  loadFnReturnTypes(G);
  loadVarMut(G);
  return G;
}

const RustType *builtinVarType(StringRef Name) {
  static const RustType LocalErrno = RustType::parse("i32");
  static const RustType ErrnoRef = RustType::parse("&mut i32");
  if (Name == "_xj_local_errno")
    return &LocalErrno;
  if (Name == "_xj_errno")
    return &ErrnoRef;
  return nullptr;
}

} // namespace xj
