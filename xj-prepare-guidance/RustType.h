// A parsed Rust type from guidance, and the questions the pass asks of it.
//
// Guidance names Rust types as strings (`&mut [u8]`, `Vec<Vec<c_int>>`,
// `Option<&'static u8>`). The pass never generates Rust; it only needs to
// know which family a type belongs to, because the family decides how a C
// pointer that carries it is read: as a whole buffer, as one element, or as
// an opaque owned value. Anything the parser does not understand is kept as
// opaque text and treated as an owned value.

#pragma once

#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

namespace xj {

struct RustType {
  enum class Kind { Path, Ref, RawPtr, Slice, Array, Tuple, Opaque };

  Kind K = Kind::Opaque;
  // Ref and RawPtr.
  bool Mut = false;
  // Ref only; kept so the printed form round-trips.
  std::string Lifetime;
  // Path: segments joined by `::`, with a leading `::` when written.
  // Opaque: the original text.
  std::string Name;
  // Path generics; the single element of Ref, RawPtr, Slice and Array.
  std::vector<RustType> Args;
  // Array length expression text.
  std::string Len;

  static RustType parse(llvm::StringRef Text);

  std::string str() const;
  // The type as an identifier fragment, for naming what carries it:
  // `&mut [u8]` is `ref_mut_slice_u8`, `Vec<::core::ffi::c_int>` is
  // `Vec_c_int`, `[u8; 4]` is `array_4_u8`. Lifetimes and path prefixes are
  // dropped; a length never ends the fragment, so it does not read as a
  // suffix.
  std::string identifier() const;
  llvm::StringRef lastSegment() const;

  bool isPath(llvm::StringRef Segment) const;
  bool isRef() const { return K == Kind::Ref; }
  bool isStr() const { return isPath("str"); }
  bool isString() const { return isPath("String"); }
  bool isVec() const { return isPath("Vec") && Args.size() == 1; }
  bool isBox() const { return isPath("Box") && Args.size() == 1; }
  bool isOption() const { return isPath("Option") && Args.size() == 1; }

  // `u8`, `f64`, `::core::ffi::c_int`, `libc::size_t`, `bool`, ...
  bool isScalarNumeric() const;

  // The pointee of a `&T`, or this type.
  const RustType &stripRefs() const;

  // The element of a buffer: `[T]`, `[T; N]`, `Vec<T>`, `Box<[T]>`, each
  // possibly behind references. Null when this is not a buffer.
  const RustType *bufferElement() const;
  bool isBuffer() const { return bufferElement() != nullptr; }

  // `&[T]` / `&mut [T]`: a borrowed suffix of a buffer.
  bool isSliceRef() const;
  // `&T` / `&mut T` / `Box<T>` naming one object, not a buffer or a string.
  bool isSingleObject() const;
  // Whether a borrowed element sink (`&T`, `&mut T`) can take a place.
  bool isElementRef() const { return isRef() && isSingleObject(); }

  // Carries guidance of its own, so a nested C pointer needs a typedef:
  // references, buffers, strings, boxes and options. Scalars, raw pointers
  // and plain record names do not.
  bool isGuidedLike() const;
};

// `Text` with each run of characters that cannot appear in a C identifier
// replaced by `_`, and without leading or trailing `_`.
std::string identifierFragment(llvm::StringRef Text);

} // namespace xj
