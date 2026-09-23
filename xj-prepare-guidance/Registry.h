// Run-wide registry of marker typedefs and marker functions.
//
// A key is one definition: flows and declarations that need the same one
// share it. Its name says what it is, `xj_ty_ref_slice_u8` for a typedef
// guided as `&[u8]`, `xj_coerce_String_to_ref_str` for a coercion; keys that
// would read the same get `_1`, `_2`, ... in sorted key order.
//
// Names must agree across translation units, because refolding moves an
// edit into a shared header only when every TU made the same edit. So names
// are not handed out as keys are met: the first sweep interns every key,
// `finalize` names them, and the second sweep asks for the names. Spellings
// may mention a typedef before it has a name; such references are written
// as placeholders and resolved on output.

#pragma once

#include "RustType.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace xj {

// A C type written so it can appear outside any function.
struct Spelling {
  std::string Text;
  // Mentions only builtins and header typedefs, so it is valid at the top
  // of the force-included header.
  bool HeaderSafe = true;
  // Marker typedefs the text mentions, by key id.
  std::set<std::string> Typedefs;
};

// Placeholder for the name of the typedef with key `Id`.
std::string typedefRef(llvm::StringRef Id);

// A key id for `Text`; ids never contain placeholder delimiters, so an id
// can itself sit inside a placeholder.
std::string keyId(llvm::StringRef Family, llvm::StringRef Text);

struct TypedefKey {
  std::string Id;
  RustType Rust;
  // `typedef <declarator>;` with `@NAME@` where the typedef name goes.
  Spelling Decl;
  // The element typedef of a nested buffer (`Vec<Vec<u8>>` on `char **`).
  std::string ElementId;
};

enum class Family { SliceAll, SliceFrom, ElemRef, Index, Coerce, IsNull };

llvm::StringRef familyName(Family F);

struct MarkerKey {
  std::string Id;
  Family F;
  // `R` of `static inline R name(P)`.
  Spelling Ret;
  // The parameter declarator, `P b`.
  Spelling Param;
  // Return statement body, e.g. `(R)(b + i)`.
  std::string Body;
  std::string FromRust;
  std::string ToRust;
  // The name the marker gets unless another marker would read the same.
  std::string Base;
};

class Registry {
public:
  const TypedefKey &intern(TypedefKey K);
  const MarkerKey &intern(MarkerKey K);
  const TypedefKey *typedefById(llvm::StringRef Id) const;

  void finalize();
  bool isFinal() const { return Final; }
  bool empty() const { return Typedefs.empty() && Markers.empty(); }

  std::string name(const TypedefKey &K) const;
  std::string name(const MarkerKey &K) const;
  std::string resolve(llvm::StringRef Text) const;

  bool isHeaderSafe(const TypedefKey &K) const;
  bool isHeaderSafe(const MarkerKey &K) const;
  std::string definition(const TypedefKey &K) const;
  std::string definition(const MarkerKey &K) const;

  std::string headerText() const;
  llvm::json::Object markerTypedefsJson() const;
  llvm::json::Object markersJson() const;

  // Local typedefs a local definition depends on, innermost first.
  std::vector<const TypedefKey *> typedefClosure(const Spelling &S) const;

private:
  std::map<std::string, TypedefKey> Typedefs;
  std::map<std::string, MarkerKey> Markers;
  std::map<std::string, std::string> Names;
  std::set<std::string> Taken;
  bool Final = false;

  void nameLate(const std::string &Id, const std::string &Base);
  std::string freeName(const std::string &Base);
  void appendHeaderTypedef(const TypedefKey &K, std::set<std::string> &Done,
                           std::string &Out) const;
};

} // namespace xj
