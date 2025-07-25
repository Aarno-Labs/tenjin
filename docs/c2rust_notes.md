# Miscellaneous c2rust Notes

## Syntax/Translation Examples

### Arrays and Pointer Decay

Given code like

```c
char s[10] = "";
puts(s);
```

c2rust will produce

```rs
let mut s: [libc::c_char; 10] = ...transmute...(b"\0\0\0\0\0\0\0\0\0\0");
puts(s.as_mut_ptr());
```

The intermediate TypedAstContext will have entries such as

```
        CDeclId(
            452,
        ): Located {
            loc: Some(
                SrcSpan {
                    fileid: 10,
                    begin_line: 632,
                    begin_column: 1,
                    end_line: 632,
                    end_column: 33,
                },
            ),
            kind: Function {
                is_global: true,
                is_inline: false,
                is_implicit: false,
                is_extern: true,
                is_inline_externally_visible: false,
                typ: CTypeId(
                    227,
                ),
                name: "puts",
                parameters: [
                    CDeclId(
                        453,
                    ),
                ],
                body: None,
                attrs: {},
            },
        },
...
        CExprId(
            854,
        ): Located {
            loc: Some(
                SrcSpan {
                    fileid: 16,
                    begin_line: 40,
                    begin_column: 5,
                    end_line: 40,
                    end_column: 5,
                },
            ),
            kind: DeclRef(
                CQualTypeId {
                    qualifiers: Qualifiers {
                        is_const: false,
                        is_restrict: false,
                        is_volatile: false,
                    },
                    ctype: CTypeId(
                        227,
                    ),
                },
                CDeclId(
                    452,
                ),
                RValue,
            ),
        },
...
        CExprId(
            848,
        ): Located {
            loc: Some(
                SrcSpan {
                    fileid: 16,
                    begin_line: 40,
                    begin_column: 5,
                    end_line: 40,
                    end_column: 12,
                },
            ),
            kind: Call(
                CQualTypeId {
                    qualifiers: Qualifiers {
                        is_const: false,
                        is_restrict: false,
                        is_volatile: false,
                    },
                    ctype: CTypeId(
                        15,
                    ),
                },
                CExprId(
                    846,
                ),
                [
                    CExprId(
                        847,
                    ),
                ],
            ),
        },

```

along with more for `FunctionToPointerDecay` and `ImplicitCast`. This output can be viewed by passing the `--dump-typed-clang-ast` flag to `c2rust transpile`.

The structure can be viewed more compactly with `10j clang-ast-xml <SOME_FILE.c>`, for which the `puts(s)` statement will be shown with

```
...
    |-CallExpr 0x5dd19a7a0298 <line:40:5, col:12> 'int'
    | |-ImplicitCastExpr 0x5dd19a7a0280 <col:5> 'int (*)(const char *)' <FunctionToPointerDecay>
    | | `-DeclRefExpr 0x5dd19a7a0208 <col:5> 'int (const char *)' Function 0x5dd19a77fea0 'puts' 'int (const char *)'
    | `-ImplicitCastExpr 0x5dd19a7a02d8 <col:10> 'const char *' <NoOp>
    |   `-ImplicitCastExpr 0x5dd19a7a02c0 <col:10> 'char *' <ArrayToPointerDecay>
    |     `-DeclRefExpr 0x5dd19a7a0228 <col:10> 'char[100]' lvalue Var 0x5dd19a79f730 's1' 'char[100]'
...
```

Anyways, notice that in the Rust output line

```rs
puts(s.as_mut_ptr());
```

the `s` output comes from the `DeclRef` case of `convert_expr` in `c2rust-transpile/src/translator/mod.rs`, whereas
the `as_mut_ptr()` comes from the `ArrayToPointerDecay` case of `convert_cast`.


# Takeaways

- Syntax that has a direct mapping in context (`s` => `s.as_mut_ptr()`) may be produced by fragmented means.
  This has some implications:
  1. To obtain a particular result in the translated code may require coordinated modifications across multiple production sites.
  1. Translating by matching on produced Rust code avoids the need to account for every potential source of syntax; one must only
     instead account for the possible productions of syntax.
  1. The emission of particular fragments of Rust syntax (e.g. `.as_mut_ptr()`) is dependent on **implicit** assumptions about
     representation choices as proxied by the structure of the Clang AST. 

Also:
  - Rust syn::Expr is anonymous, unlike the basis CExprId. This makes it harder to give guidance in reference to produced Rust code.
