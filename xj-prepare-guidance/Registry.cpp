#include "Registry.h"

#include "llvm/Support/raw_ostream.h"

#include <algorithm>

using llvm::StringRef;

namespace xj {

static constexpr char RefOpen = '\x01';
static constexpr char RefClose = '\x02';

std::string typedefRef(StringRef Id) {
  return std::string(1, RefOpen) + Id.str() + std::string(1, RefClose);
}

std::string keyId(StringRef Family, StringRef Text) {
  std::string Id = Family.str() + "|";
  for (char C : Text)
    Id += C == RefOpen ? '<' : C == RefClose ? '>' : C;
  return Id;
}

StringRef familyName(Family F) {
  switch (F) {
  case Family::SliceAll:
    return "slice_all";
  case Family::SliceFrom:
    return "slice_from";
  case Family::ElemRef:
    return "elem_ref";
  case Family::Index:
    return "index";
  case Family::Coerce:
    return "coerce";
  case Family::IsNull:
    return "is_null";
  }
  return "unknown";
}

static std::string markerPrefix(Family F) {
  return "xj_" + familyName(F).str() + "_";
}

const TypedefKey &Registry::intern(TypedefKey K) {
  auto [It, Inserted] = Typedefs.emplace(K.Id, std::move(K));
  if (Inserted && Final)
    nameLate(It->first, "xj_ty_");
  return It->second;
}

const MarkerKey &Registry::intern(MarkerKey K) {
  auto [It, Inserted] = Markers.emplace(K.Id, std::move(K));
  if (Inserted && Final)
    nameLate(It->first, markerPrefix(It->second.F));
  return It->second;
}

const TypedefKey *Registry::typedefById(StringRef Id) const {
  auto It = Typedefs.find(Id.str());
  return It == Typedefs.end() ? nullptr : &It->second;
}

void Registry::finalize() {
  unsigned N = 0;
  for (const auto &[Id, K] : Typedefs)
    Names[Id] = "xj_ty_" + std::to_string(N++);
  std::map<Family, unsigned> PerFamily;
  for (const auto &[Id, K] : Markers)
    Names[Id] = markerPrefix(K.F) + std::to_string(PerFamily[K.F]++);
  Final = true;
}

// A key the first sweep did not see means the two sweeps disagreed; the
// name is still unique, only no longer in sorted order.
void Registry::nameLate(const std::string &Id, StringRef Prefix) {
  unsigned N = 0;
  for (const auto &[_, Name] : Names)
    if (StringRef(Name).starts_with(Prefix))
      ++N;
  Names[Id] = Prefix.str() + std::to_string(N);
  llvm::errs() << "xj-prepare-guidance: warning: late key " << Names[Id]
               << "\n";
}

std::string Registry::name(const TypedefKey &K) const {
  auto It = Names.find(K.Id);
  return It == Names.end() ? "xj_ty_unnamed" : It->second;
}

std::string Registry::name(const MarkerKey &K) const {
  auto It = Names.find(K.Id);
  return It == Names.end() ? "xj_marker_unnamed" : It->second;
}

std::string Registry::resolve(StringRef Text) const {
  std::string Out;
  while (true) {
    size_t Open = Text.find(RefOpen);
    if (Open == StringRef::npos)
      return Out + Text.str();
    size_t Close = Text.find(RefClose, Open);
    Out += Text.take_front(Open);
    auto It = Names.find(Text.slice(Open + 1, Close).str());
    Out += It == Names.end() ? "xj_ty_unnamed" : It->second;
    Text = Text.drop_front(Close + 1);
  }
}

bool Registry::isHeaderSafe(const TypedefKey &K) const {
  if (!K.Decl.HeaderSafe)
    return false;
  for (const std::string &Id : K.Decl.Typedefs)
    if (const TypedefKey *Inner = typedefById(Id))
      if (!isHeaderSafe(*Inner))
        return false;
  return true;
}

static bool spellingHeaderSafe(const Registry &R, const Spelling &S) {
  if (!S.HeaderSafe)
    return false;
  for (const std::string &Id : S.Typedefs)
    if (const TypedefKey *K = R.typedefById(Id))
      if (!R.isHeaderSafe(*K))
        return false;
  return true;
}

bool Registry::isHeaderSafe(const MarkerKey &K) const {
  return spellingHeaderSafe(*this, K.Ret) && spellingHeaderSafe(*this, K.Param);
}

std::string Registry::definition(const TypedefKey &K) const {
  std::string Text = resolve(K.Decl.Text);
  size_t At = Text.find("@NAME@");
  if (At != std::string::npos)
    Text.replace(At, 6, name(K));
  return Text;
}

static StringRef extraParams(Family F) {
  bool Indexed =
      F == Family::SliceFrom || F == Family::ElemRef || F == Family::Index;
  return Indexed ? ", long i" : "";
}

std::string Registry::definition(const MarkerKey &K) const {
  std::string Ret = resolve(K.Ret.Text);
  std::string Space = StringRef(Ret).ends_with("*") ? "" : " ";
  return "static inline " + Ret + Space + name(K) + "(" +
         resolve(K.Param.Text) + extraParams(K.F).str() + ") { return " +
         resolve(K.Body) + "; }";
}

std::vector<const TypedefKey *>
Registry::typedefClosure(const Spelling &S) const {
  std::vector<const TypedefKey *> Out;
  std::set<std::string> Seen;
  std::vector<std::string> Work(S.Typedefs.begin(), S.Typedefs.end());
  while (!Work.empty()) {
    std::string Id = Work.back();
    Work.pop_back();
    const TypedefKey *K = typedefById(Id);
    if (!K || !Seen.insert(Id).second)
      continue;
    Out.push_back(K);
    Work.insert(Work.end(), K->Decl.Typedefs.begin(), K->Decl.Typedefs.end());
  }
  // Dependencies were pushed after their users.
  std::reverse(Out.begin(), Out.end());
  return Out;
}

void Registry::appendHeaderTypedef(const TypedefKey &K,
                                   std::set<std::string> &Done,
                                   std::string &Out) const {
  if (!Done.insert(K.Id).second)
    return;
  for (const std::string &Id : K.Decl.Typedefs)
    if (const TypedefKey *Inner = typedefById(Id))
      appendHeaderTypedef(*Inner, Done, Out);
  Out += definition(K) + "\n";
}

std::string Registry::headerText() const {
  std::string Out = "/* Generated by xj-prepare-guidance; do not edit. */\n"
                    "#ifndef XJ_GUIDANCE_H\n#define XJ_GUIDANCE_H\n";
  std::set<std::string> Done;
  for (const auto &[_, K] : Typedefs)
    if (isHeaderSafe(K))
      appendHeaderTypedef(K, Done, Out);
  // Sorted by name, which is the order readers expect.
  std::map<std::string, const MarkerKey *> ByName;
  for (const auto &[_, K] : Markers)
    if (isHeaderSafe(K))
      ByName[name(K)] = &K;
  for (const auto &[_, K] : ByName)
    Out += definition(*K) + "\n";
  Out += "#endif\n";
  return Out;
}

llvm::json::Object Registry::markerTypedefsJson() const {
  llvm::json::Object O;
  for (const auto &[_, K] : Typedefs)
    O[name(K)] = K.Rust.str();
  return O;
}

llvm::json::Object Registry::markersJson() const {
  llvm::json::Object O;
  for (const auto &[_, K] : Markers)
    O[name(K)] = llvm::json::Object{{"family", familyName(K.F)},
                                    {"from", K.FromRust},
                                    {"to", K.ToRust}};
  return O;
}

} // namespace xj
