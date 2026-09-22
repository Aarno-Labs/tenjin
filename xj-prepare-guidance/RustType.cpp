#include "RustType.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"

#include <cctype>
#include <optional>

using llvm::StringRef;

namespace xj {

namespace {

struct Token {
  enum Kind { Ident, Lifetime, PathSep, Punct, Other, End };
  Kind K;
  std::string Text;
  size_t Begin;
};

std::vector<Token> tokenize(StringRef S) {
  std::vector<Token> Toks;
  size_t I = 0;
  auto isIdentChar = [](char C) { return std::isalnum(C) || C == '_'; };
  while (I < S.size()) {
    char C = S[I];
    if (std::isspace(C)) {
      ++I;
      continue;
    }
    size_t Begin = I;
    if (std::isalpha(C) || C == '_' || std::isdigit(C)) {
      while (I < S.size() && isIdentChar(S[I]))
        ++I;
      Toks.push_back({Token::Ident, S.slice(Begin, I).str(), Begin});
    } else if (C == '\'') {
      ++I;
      while (I < S.size() && isIdentChar(S[I]))
        ++I;
      Toks.push_back({Token::Lifetime, S.slice(Begin, I).str(), Begin});
    } else if (S.substr(I).starts_with("::")) {
      I += 2;
      Toks.push_back({Token::PathSep, "::", Begin});
    } else if (StringRef("&*[];<>,()").contains(C)) {
      ++I;
      Toks.push_back({Token::Punct, std::string(1, C), Begin});
    } else {
      ++I;
      Toks.push_back({Token::Other, std::string(1, C), Begin});
    }
  }
  Toks.push_back({Token::End, "", S.size()});
  return Toks;
}

// Recursive descent over the subset of Rust type syntax guidance uses.
class Parser {
public:
  Parser(StringRef Text) : Text(Text), Toks(tokenize(Text)) {}

  std::optional<RustType> parseAll() {
    auto T = parseType();
    if (!T || peek().K != Token::End)
      return std::nullopt;
    return T;
  }

private:
  StringRef Text;
  std::vector<Token> Toks;
  size_t Pos = 0;

  const Token &peek() const { return Toks[Pos]; }
  bool isPunct(StringRef P) const {
    return peek().K == Token::Punct && peek().Text == P;
  }
  bool isKeyword(StringRef W) const {
    return peek().K == Token::Ident && peek().Text == W;
  }
  bool accept(StringRef P) {
    if (!isPunct(P))
      return false;
    ++Pos;
    return true;
  }

  std::optional<RustType> parseType() {
    if (isPunct("&"))
      return parseRef();
    if (isPunct("*"))
      return parseRawPtr();
    if (isPunct("["))
      return parseSliceOrArray();
    if (isPunct("("))
      return parseTuple();
    if (peek().K == Token::Ident || peek().K == Token::PathSep)
      return parsePath();
    return std::nullopt;
  }

  std::optional<RustType> parseRef() {
    ++Pos;
    RustType T;
    T.K = RustType::Kind::Ref;
    if (peek().K == Token::Lifetime)
      T.Lifetime = Toks[Pos++].Text;
    if (isKeyword("mut")) {
      T.Mut = true;
      ++Pos;
    }
    auto Inner = parseType();
    if (!Inner)
      return std::nullopt;
    T.Args.push_back(std::move(*Inner));
    return T;
  }

  std::optional<RustType> parseRawPtr() {
    ++Pos;
    RustType T;
    T.K = RustType::Kind::RawPtr;
    if (isKeyword("mut"))
      T.Mut = true;
    else if (!isKeyword("const"))
      return std::nullopt;
    ++Pos;
    auto Inner = parseType();
    if (!Inner)
      return std::nullopt;
    T.Args.push_back(std::move(*Inner));
    return T;
  }

  std::optional<RustType> parseSliceOrArray() {
    ++Pos;
    auto Inner = parseType();
    if (!Inner)
      return std::nullopt;
    RustType T;
    T.Args.push_back(std::move(*Inner));
    if (accept("]")) {
      T.K = RustType::Kind::Slice;
      return T;
    }
    if (!accept(";"))
      return std::nullopt;
    // The length is an expression; keep its text up to the matching `]`.
    size_t Begin = peek().Begin;
    int Depth = 0;
    while (peek().K != Token::End) {
      if (isPunct("["))
        ++Depth;
      if (isPunct("]")) {
        if (Depth == 0)
          break;
        --Depth;
      }
      ++Pos;
    }
    if (!isPunct("]"))
      return std::nullopt;
    T.Len = Text.slice(Begin, peek().Begin).trim().str();
    ++Pos;
    T.K = RustType::Kind::Array;
    return T;
  }

  std::optional<RustType> parseTuple() {
    ++Pos;
    RustType T;
    T.K = RustType::Kind::Tuple;
    while (!isPunct(")")) {
      auto Elem = parseType();
      if (!Elem)
        return std::nullopt;
      T.Args.push_back(std::move(*Elem));
      if (!accept(","))
        break;
    }
    if (!accept(")"))
      return std::nullopt;
    return T;
  }

  std::optional<RustType> parsePath() {
    RustType T;
    T.K = RustType::Kind::Path;
    if (peek().K == Token::PathSep) {
      T.Name = "::";
      ++Pos;
    }
    while (true) {
      if (peek().K != Token::Ident)
        return std::nullopt;
      StringRef W = peek().Text;
      if (W == "dyn" || W == "impl" || W == "fn")
        return std::nullopt;
      T.Name += W;
      ++Pos;
      if (peek().K != Token::PathSep)
        break;
      T.Name += "::";
      ++Pos;
    }
    if (accept("<")) {
      while (!isPunct(">")) {
        if (peek().K == Token::Lifetime) {
          RustType L;
          L.K = RustType::Kind::Path;
          L.Name = Toks[Pos++].Text;
          T.Args.push_back(std::move(L));
        } else {
          auto Arg = parseType();
          if (!Arg)
            return std::nullopt;
          T.Args.push_back(std::move(*Arg));
        }
        if (!accept(","))
          break;
      }
      if (!accept(">"))
        return std::nullopt;
    }
    return T;
  }
};

std::string collapseSpaces(StringRef S) {
  std::string Out;
  bool PendingSpace = false;
  for (char C : S.trim()) {
    if (std::isspace(C)) {
      PendingSpace = true;
      continue;
    }
    if (PendingSpace && !Out.empty())
      Out += ' ';
    PendingSpace = false;
    Out += C;
  }
  return Out;
}

std::string joinArgs(const std::vector<RustType> &Args) {
  std::string Out;
  for (size_t I = 0; I < Args.size(); ++I) {
    if (I)
      Out += ", ";
    Out += Args[I].str();
  }
  return Out;
}

} // namespace

RustType RustType::parse(StringRef Text) {
  if (auto T = Parser(Text).parseAll())
    return *T;
  RustType Opaque;
  Opaque.Name = collapseSpaces(Text);
  return Opaque;
}

std::string RustType::str() const {
  switch (K) {
  case Kind::Path:
    return Args.empty() ? Name : Name + "<" + joinArgs(Args) + ">";
  case Kind::Ref:
    return "&" + (Lifetime.empty() ? "" : Lifetime + " ") +
           (Mut ? "mut " : "") + Args[0].str();
  case Kind::RawPtr:
    return std::string(Mut ? "*mut " : "*const ") + Args[0].str();
  case Kind::Slice:
    return "[" + Args[0].str() + "]";
  case Kind::Array:
    return "[" + Args[0].str() + "; " + Len + "]";
  case Kind::Tuple:
    return "(" + joinArgs(Args) + (Args.size() == 1 ? ",)" : ")");
  case Kind::Opaque:
    return Name;
  }
  return Name;
}

StringRef RustType::lastSegment() const {
  if (K != Kind::Path)
    return "";
  StringRef Last = StringRef(Name).rsplit("::").second;
  return Last.empty() ? StringRef(Name) : Last;
}

bool RustType::isPath(StringRef Segment) const {
  return K == Kind::Path && lastSegment() == Segment;
}

bool RustType::isScalarNumeric() const {
  if (K != Kind::Path || !Args.empty())
    return false;
  StringRef Seg = lastSegment();
  if (Seg == "c_void")
    return false;
  if (Seg.starts_with("c_"))
    return true;
  return llvm::StringSwitch<bool>(Seg)
      .Cases("u8", "u16", "u32", "u64", "u128", "usize", true)
      .Cases("i8", "i16", "i32", "i64", "i128", "isize", true)
      .Cases("f32", "f64", "bool", true)
      .Cases("size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t", true)
      .Default(false);
}

const RustType &RustType::stripRefs() const {
  const RustType *T = this;
  while (T->K == Kind::Ref)
    T = &T->Args[0];
  return *T;
}

const RustType *RustType::bufferElement() const {
  const RustType &T = stripRefs();
  if (T.K == Kind::Slice || T.K == Kind::Array)
    return &T.Args[0];
  if (T.isVec())
    return &T.Args[0];
  if (T.isBox() && T.Args[0].K == Kind::Slice)
    return &T.Args[0].Args[0];
  return nullptr;
}

bool RustType::isSliceRef() const {
  return K == Kind::Ref && Args[0].K == Kind::Slice;
}

bool RustType::isSingleObject() const {
  if (K != Kind::Ref && !isBox())
    return false;
  const RustType &Target = Args[0];
  return !Target.isBuffer() && !Target.isStr() && !Target.isString();
}

bool RustType::isGuidedLike() const {
  switch (K) {
  case Kind::Ref:
  case Kind::Slice:
  case Kind::Array:
    return true;
  case Kind::Path:
    return isString() || isStr() || isVec() || isBox() || isOption();
  default:
    return false;
  }
}

} // namespace xj
