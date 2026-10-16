//! Whole-crate time type mapping followed by local argument lifting.
//!
//! Type mapping precedes expression rewrites so records, arrays, sizeof and
//! foreign signatures all refer to the same layout-compatible types. Static
//! results require a separate block-level escape and overwrite check.
use crate::{Rewriter, SymbolTable};
use quote::ToTokens;
use std::{
    collections::{HashMap, HashSet},
    path::PathBuf,
};
use syn::{
    Expr, Item, Member, Pat, Stmt, Type, parse_quote,
    visit_mut::{self, VisitMut},
};

#[derive(Clone, Copy)]
enum Arg {
    Value,
    Input,
    Output,
    Optional,
    Bytes,
    Buffer,
}
use Arg::*;
fn args(name: &str) -> Option<&'static [Arg]> {
    Some(match name {
        "clock" | "tzset" | "tzname" | "timezone" | "daylight" => &[],
        "difftime" => &[Value, Value],
        "time" => &[Optional],
        "mktime" | "timegm" => &[Output],
        "localtime" | "gmtime" | "asctime" | "ctime" => &[Input],
        "localtime_r" | "gmtime_r" => &[Input, Output],
        "strftime" => &[Buffer, Value, Bytes, Input],
        "strptime" => &[Bytes, Bytes, Output],
        "asctime_r" | "ctime_r" => &[Input, Buffer],
        "clock_gettime" | "clock_getcpuclockid" => &[Value, Output],
        "clock_getres" => &[Value, Optional],
        "clock_settime" => &[Value, Input],
        "timespec_get" => &[Output, Value],
        "nanosleep" => &[Input, Optional],
        "clock_nanosleep" => &[Value, Value, Input, Optional],
        _ => return None,
    })
}
fn unparen(mut e: &Expr) -> &Expr {
    loop {
        match e {
            Expr::Paren(p) => e = &p.expr,
            Expr::Group(g) => e = &g.expr,
            _ => return e,
        }
    }
}
fn base(mut e: &Expr) -> &Expr {
    loop {
        match unparen(e) {
            Expr::Cast(c) if matches!(&*c.ty, Type::Ptr(_)) => e = &c.expr,
            Expr::MethodCall(m) if m.method == "cast" && m.args.is_empty() => e = &m.receiver,
            other => return other,
        }
    }
}
fn name(e: &Expr) -> Option<String> {
    if let Expr::Path(p) = unparen(e) {
        p.path.get_ident().map(ToString::to_string)
    } else {
        None
    }
}
fn last_type(t: &Type) -> Option<String> {
    match t {
        Type::Path(p) => p.path.segments.last().map(|s| s.ident.to_string()),
        _ => None,
    }
}
fn resolved<'a>(mut t: &'a Type, symbols: &'a SymbolTable) -> &'a Type {
    for _ in 0..symbols.type_aliases.len() + 1 {
        let Some(next) = (if let Type::Path(p) = t {
            p.path.get_ident().map(ToString::to_string)
        } else {
            None
        })
        .and_then(|n| symbols.type_aliases.get(&n))
        .and_then(Option::as_ref) else {
            break;
        };
        t = next;
    }
    t
}
fn pointer(t: &Type, mutable: bool, symbols: &SymbolTable) -> bool {
    matches!(resolved(t, symbols), Type::Ptr(p) if !mutable || p.mutability.is_some())
}
fn scalar(t: &Type, kind: &str, symbols: &SymbolTable) -> bool {
    let t = resolved(t, symbols);
    let n = last_type(t).unwrap_or_default();
    match kind {
        "int" => matches!(n.as_str(), "c_int" | "i32" | "pid_t"),
        "clockid" => matches!(n.as_str(), "c_int" | "i32" | "c_uint" | "u32" | "clockid_t"),
        "long" => matches!(n.as_str(), "c_long" | "i64" | "i32"),
        "time" => matches!(n.as_str(), "time_t" | "__time_t" | "c_long" | "i64" | "i32"),
        "clock" => matches!(
            n.as_str(),
            "clock_t" | "__clock_t" | "c_long" | "i64" | "i32" | "u64" | "u32"
        ),
        "size" => matches!(n.as_str(), "usize" | "size_t"),
        "double" => matches!(n.as_str(), "c_double" | "f64"),
        "char" => matches!(n.as_str(), "c_char" | "u8" | "i8"),
        "tm" => matches!(n.as_str(), "tm" | "Tm"),
        "timespec" => matches!(n.as_str(), "timespec" | "Timespec"),
        _ => false,
    }
}
fn ctime_signature(sig: &syn::Signature, symbols: &SymbolTable) -> bool {
    let n = sig.ident.to_string();
    let Some(roles) = args(&n) else { return false };
    let kinds: &[&str] = match n.as_str() {
        "clock" | "tzset" | "timezone" | "daylight" => &[],
        "difftime" => &["time", "time"],
        "time" | "localtime" | "gmtime" | "ctime" => &["time"],
        "mktime" | "timegm" | "asctime" => &["tm"],
        "localtime_r" | "gmtime_r" => &["time", "tm"],
        "strftime" => &["char", "size", "char", "tm"],
        "strptime" => &["char", "char", "tm"],
        "asctime_r" => &["tm", "char"],
        "ctime_r" => &["time", "char"],
        "clock_gettime" | "clock_getres" | "clock_settime" => &["clockid", "timespec"],
        "clock_getcpuclockid" => &["int", "clockid"],
        "timespec_get" => &["timespec", "int"],
        "nanosleep" => &["timespec", "timespec"],
        "clock_nanosleep" => &["clockid", "int", "timespec", "timespec"],
        _ => return false,
    };
    if sig.variadic.is_some() || sig.inputs.len() != roles.len() {
        return false;
    }
    for ((arg, role), kind) in sig.inputs.iter().zip(roles).zip(kinds) {
        let syn::FnArg::Typed(arg) = arg else {
            return false;
        };
        let t = resolved(&arg.ty, symbols);
        let value = match role {
            Input | Bytes if pointer(t, false, symbols) => {
                if let Type::Ptr(p) = t {
                    &*p.elem
                } else {
                    return false;
                }
            }
            Output | Optional | Buffer if pointer(t, true, symbols) => {
                if let Type::Ptr(p) = t {
                    &*p.elem
                } else {
                    return false;
                }
            }
            Value => t,
            _ => return false,
        };
        if !scalar(value, kind, symbols) {
            return false;
        }
    }
    let syn::ReturnType::Type(_, ret) = &sig.output else {
        return n == "tzset";
    };
    let ret = resolved(ret, symbols);
    let (kind, ptr) = match n.as_str() {
        "clock" => ("clock", false),
        "time" | "mktime" | "timegm" => ("time", false),
        "difftime" => ("double", false),
        "timezone" => ("long", false),
        "localtime" | "gmtime" | "localtime_r" | "gmtime_r" => ("tm", true),
        "strftime" => ("size", false),
        "strptime" | "asctime" | "ctime" | "asctime_r" | "ctime_r" => ("char", true),
        _ => ("int", false),
    };
    if ptr {
        matches!(ret, Type::Ptr(p) if p.mutability.is_some() && scalar(&p.elem,kind,symbols))
    } else {
        scalar(ret, kind, symbols)
    }
}
fn c_char() -> Type {
    parse_quote!(::core::ffi::c_char)
}

// C expressions have concrete record types even when the receiver is a call,
// dereference, or indexed member. Keep this inference local to the time pass.
fn expression_type(expr: &Expr, symbols: &SymbolTable) -> Option<Type> {
    fn pointee(ty: Type, symbols: &SymbolTable) -> Type {
        match resolved(&ty, symbols) {
            Type::Reference(r) => pointee((*r.elem).clone(), symbols),
            Type::Ptr(p) => pointee((*p.elem).clone(), symbols),
            ty => ty.clone(),
        }
    }
    match unparen(expr) {
        Expr::Field(f) => {
            let ty = pointee(expression_type(&f.base, symbols)?, symbols);
            let Member::Named(member) = &f.member else {
                return None;
            };
            symbols
                .fields
                .get(&last_type(&ty)?)?
                .get(&member.to_string())
                .cloned()
        }
        Expr::Index(i) => {
            let ty = pointee(expression_type(&i.expr, symbols)?, symbols);
            match ty {
                Type::Array(a) => Some(*a.elem),
                Type::Slice(s) => Some(*s.elem),
                Type::Path(p) if p.path.segments.last().is_some_and(|s| s.ident == "Vec") => {
                    let syn::PathArguments::AngleBracketed(a) = &p.path.segments.last()?.arguments
                    else {
                        return None;
                    };
                    a.args.iter().find_map(|a| {
                        if let syn::GenericArgument::Type(t) = a {
                            Some(t.clone())
                        } else {
                            None
                        }
                    })
                }
                _ => None,
            }
        }
        Expr::Unary(u) if matches!(u.op, syn::UnOp::Deref(_)) => {
            let ty = expression_type(&u.expr, symbols)?;
            match resolved(&ty, symbols) {
                Type::Ptr(p) => Some((*p.elem).clone()),
                Type::Reference(r) => Some((*r.elem).clone()),
                _ => None,
            }
        }
        Expr::Struct(s) => Some(Type::Path(syn::TypePath {
            qself: s.qself.clone(),
            path: s.path.clone(),
        })),
        Expr::RawAddr(r) => {
            let ty = expression_type(&r.expr, symbols)?;
            Some(if matches!(r.mutability, syn::PointerMutability::Mut(_)) {
                parse_quote!(*mut #ty)
            } else {
                parse_quote!(*const #ty)
            })
        }
        Expr::MethodCall(m)
            if matches!(
                m.method.to_string().as_str(),
                "offset" | "wrapping_offset" | "add" | "sub" | "wrapping_add" | "wrapping_sub"
            ) =>
        {
            let ty = expression_type(&m.receiver, symbols)?;
            matches!(resolved(&ty, symbols), Type::Ptr(_)).then_some(ty)
        }
        Expr::MethodCall(m) if m.method == "as_ptr" || m.method == "as_mut_ptr" => {
            let ty = pointee(expression_type(&m.receiver, symbols)?, symbols);
            let elem = match ty {
                Type::Array(a) => *a.elem,
                Type::Slice(s) => *s.elem,
                Type::Path(p) if p.path.segments.last().is_some_and(|s| s.ident == "Vec") => {
                    let syn::PathArguments::AngleBracketed(a) = &p.path.segments.last()?.arguments
                    else {
                        return None;
                    };
                    a.args.iter().find_map(|a| {
                        if let syn::GenericArgument::Type(t) = a {
                            Some(t.clone())
                        } else {
                            None
                        }
                    })?
                }
                _ => return None,
            };
            Some(if m.method == "as_mut_ptr" {
                parse_quote!(*mut #elem)
            } else {
                parse_quote!(*const #elem)
            })
        }
        Expr::MethodCall(m) if m.method == "as_ref" || m.method == "as_mut" => {
            let ty = expression_type(&m.receiver, symbols)?;
            let Type::Ptr(p) = resolved(&ty, symbols) else {
                return None;
            };
            let elem = &p.elem;
            Some(if m.method == "as_mut" {
                parse_quote!(Option<&mut #elem>)
            } else {
                parse_quote!(Option<&#elem>)
            })
        }
        Expr::MethodCall(m) if m.method == "unwrap" || m.method == "expect" => {
            let ty = expression_type(&m.receiver, symbols)?;
            let Type::Path(p) = resolved(&ty, symbols) else {
                return None;
            };
            let segment = p.path.segments.last()?;
            if segment.ident != "Option" {
                return None;
            }
            let syn::PathArguments::AngleBracketed(a) = &segment.arguments else {
                return None;
            };
            a.args.iter().find_map(|a| {
                if let syn::GenericArgument::Type(t) = a {
                    Some(t.clone())
                } else {
                    None
                }
            })
        }
        Expr::Call(c) => {
            if let Expr::Path(p) = unparen(&c.func)
                && let Some(n) = p.path.get_ident()
                && !symbols.has(&n.to_string())
            {
                if let Some(t) = symbols.fn_outputs.get(&n.to_string()) {
                    return Some(t.clone());
                }
                if let Some(sig) = symbols.external_fn(&n.to_string())
                    && let syn::ReturnType::Type(_, t) = &sig.output
                {
                    return Some((**t).clone());
                }
            }
            let ty = pointee(expression_type(&c.func, symbols)?, symbols);
            if let Type::BareFn(f) = ty
                && let syn::ReturnType::Type(_, t) = f.output
            {
                Some(*t)
            } else {
                None
            }
        }
        _ => crate::printf::expr_type(expr, symbols),
    }
}

fn record_local(local: &syn::Local, symbols: &mut SymbolTable) {
    let inferred = if let Pat::Ident(_) = &local.pat {
        local.init.as_ref().and_then(|init| {
            if let Expr::Struct(s) = unparen(&init.expr) {
                return Some(Type::Path(syn::TypePath {
                    qself: s.qself.clone(),
                    path: s.path.clone(),
                }));
            }
            if let Expr::Call(c) = unparen(&init.expr)
                && let Expr::Path(p) = &*c.func
            {
                if p.path.segments.len() == 1
                    && let Some(n) = p.path.get_ident()
                    && let Some(sig) = symbols.external_fn(&n.to_string())
                    && let syn::ReturnType::Type(_, t) = &sig.output
                {
                    return Some((**t).clone());
                }
                if p.path.segments.len() == 3 && p.path.segments[0].ident == "xj_ctime" {
                    return match p.path.segments[2].ident.to_string().as_str() {
                        "time" | "mktime" | "timegm" => Some(parse_quote!(::xj_ctime::time_t)),
                        "localtime" | "gmtime" if p.path.segments[1].ident == "c_abi" => {
                            Some(parse_quote!(*mut ::xj_ctime::Tm))
                        }
                        _ => None,
                    };
                }
            }
            crate::printf::expr_type(&init.expr, symbols)
        })
    } else {
        None
    };
    crate::collect_pat_bindings(&local.pat, symbols);
    if let Pat::Ident(p) = &local.pat
        && let Some(t) = inferred
    {
        symbols.insert(p.ident.to_string(), t);
    }
}

pub(crate) fn prepare(rw: &Rewriter, files: &mut [(PathBuf, syn::File)], lift: bool) {
    let mut globals = SymbolTable::from_files(files);
    fn union_fields(items: &[Item], symbols: &mut SymbolTable) {
        for item in items {
            if let Item::Union(u) = item {
                let fields = u
                    .fields
                    .named
                    .iter()
                    .filter_map(|f| f.ident.as_ref().map(|n| (n.to_string(), f.ty.clone())))
                    .collect();
                symbols.fields.insert(u.ident.to_string(), fields);
            }
            if let Item::Mod(m) = item
                && let Some((_, items)) = &m.content
            {
                union_fields(items, symbols);
            }
        }
    }
    for (_, file) in files.iter() {
        union_fields(&file.items, &mut globals);
    }
    let mut records = HashMap::new();
    let mut foreign_globals = HashMap::new();
    fn find(items: &[Item], records: &mut HashMap<String, &'static str>, symbols: &SymbolTable) {
        for item in items {
            if let Item::Struct(s) = item {
                let c_layout = s.attrs.iter().any(|a| {
                    a.path().is_ident("repr")
                        && a.parse_args::<syn::Ident>().is_ok_and(|i| i == "C")
                });
                let fields: Vec<_> = s
                    .fields
                    .iter()
                    .filter_map(|f| f.ident.as_ref().map(|i| (i.to_string(), &f.ty)))
                    .collect();
                let names: Vec<_> = fields
                    .iter()
                    .map(|(n, _)| n.trim_start_matches("__"))
                    .collect();
                let tm = [
                    "tm_sec",
                    "tm_min",
                    "tm_hour",
                    "tm_mday",
                    "tm_mon",
                    "tm_year",
                    "tm_wday",
                    "tm_yday",
                    "tm_isdst",
                    "tm_gmtoff",
                    "tm_zone",
                ];
                if c_layout
                    && s.ident == "tm"
                    && names == tm
                    && s.generics.params.is_empty()
                    && fields[..9].iter().all(|(_, t)| scalar(t, "int", symbols))
                    && scalar(fields[9].1, "long", symbols)
                    && matches!(resolved(fields[10].1,symbols), Type::Ptr(p) if scalar(&p.elem,"char",symbols))
                {
                    records.insert(s.ident.to_string(), "Tm");
                }
                if c_layout
                    && s.ident == "timespec"
                    && names == ["tv_sec", "tv_nsec"]
                    && s.generics.params.is_empty()
                    && scalar(fields[0].1, "time", symbols)
                    && scalar(fields[1].1, "long", symbols)
                {
                    records.insert(s.ident.to_string(), "Timespec");
                }
            }
            if let Item::Mod(m) = item
                && let Some((_, items)) = &m.content
            {
                find(items, records, symbols);
            }
        }
    }
    fn find_globals(items: &[Item], out: &mut HashMap<String, Type>, symbols: &SymbolTable) {
        for item in items {
            if let Item::ForeignMod(m) = item {
                for i in &m.items {
                    if let syn::ForeignItem::Static(s) = i {
                        let valid = match s.ident.to_string().as_str() {
                            "timezone" => scalar(&s.ty, "long", symbols),
                            "daylight" => scalar(&s.ty, "int", symbols),
                            "tzname" => matches!(resolved(&s.ty,symbols), Type::Array(a)
                                if matches!(base(&a.len), Expr::Lit(l) if matches!(&l.lit, syn::Lit::Int(n) if n.base10_parse::<usize>().ok() == Some(2)))
                                && matches!(resolved(&a.elem,symbols), Type::Ptr(p) if scalar(&p.elem,"char",symbols))),
                            _ => false,
                        };
                        if valid {
                            out.insert(s.ident.to_string(), (*s.ty).clone());
                        }
                    }
                }
            }
            if let Item::Mod(m) = item
                && let Some((_, items)) = &m.content
            {
                find_globals(items, out, symbols);
            }
        }
    }
    for (_, file) in files.iter() {
        find(&file.items, &mut records, &globals);
        find_globals(&file.items, &mut foreign_globals, &globals);
    }
    fn mapped_records(
        sig: &syn::Signature,
        symbols: &SymbolTable,
        records: &HashMap<String, &'static str>,
    ) -> bool {
        struct Check<'a> {
            symbols: &'a SymbolTable,
            records: &'a HashMap<String, &'static str>,
            valid: bool,
        }
        impl VisitMut for Check<'_> {
            fn visit_type_mut(&mut self, ty: &mut Type) {
                if let Type::Path(p) = resolved(ty, self.symbols)
                    && let Some(n) = p.path.segments.last()
                    && matches!(n.ident.to_string().as_str(), "tm" | "timespec")
                    && p.path.segments.first().is_none_or(|s| s.ident != "libc")
                    && !self.records.contains_key(&n.ident.to_string())
                {
                    self.valid = false;
                }
                visit_mut::visit_type_mut(self, ty);
            }
        }
        let mut check = Check {
            symbols,
            records,
            valid: true,
        };
        check.visit_signature_mut(&mut sig.clone());
        check.valid
    }
    let foreign: HashSet<_> = globals
        .foreign_fns
        .iter()
        .filter(|(n, s)| {
            !globals.defined_fns.contains(*n)
                && ctime_signature(s, &globals)
                && mapped_records(s, &globals, &records)
        })
        .map(|(n, _)| n.clone())
        .collect();
    for (path, file) in files.iter_mut() {
        *rw.cur_file.borrow_mut() = Some(path.clone());
        let mut mapper = Mapper {
            rw,
            records: &records,
            foreign: &foreign,
            symbols: globals.clone(),
            static_init: false,
            foreign_globals: &foreign_globals,
            lift,
        };
        mapper.visit_file_mut(file);
    }
    // Keep foreign declarations that are still used as function values. The
    // ordinary callee rewrites cover calls; opaque macros are conservative.
    let mut used = HashSet::new();
    struct Uses<'a>(&'a mut HashSet<String>);
    impl VisitMut for Uses<'_> {
        fn visit_expr_path_mut(&mut self, p: &mut syn::ExprPath) {
            if let Some(n) = p.path.get_ident() {
                self.0.insert(n.to_string());
            }
        }
        fn visit_macro_mut(&mut self, m: &mut syn::Macro) {
            use syn::parse::Parser;
            if let Ok(mut a) = syn::punctuated::Punctuated::<Expr, syn::Token![,]>::parse_terminated
                .parse2(m.tokens.clone())
            {
                for arg in &mut a {
                    self.visit_expr_mut(arg);
                }
                return;
            }
            // Retain declarations used by opaque, non-expression macros.
            fn tokens(ts: proc_macro2::TokenStream, names: &mut HashSet<String>) {
                for t in ts {
                    match t {
                        proc_macro2::TokenTree::Ident(i) => {
                            names.insert(i.to_string());
                        }
                        proc_macro2::TokenTree::Group(g) => tokens(g.stream(), names),
                        _ => (),
                    }
                }
            }
            tokens(m.tokens.clone(), self.0);
        }
    }
    for (_, file) in files.iter_mut() {
        Uses(&mut used).visit_file_mut(file);
    }
    struct Remove<'a> {
        foreign: &'a HashSet<String>,
        globals: &'a HashMap<String, Type>,
        used: &'a HashSet<String>,
    }
    impl VisitMut for Remove<'_> {
        fn visit_item_foreign_mod_mut(&mut self, item: &mut syn::ItemForeignMod) {
            item.items.retain(|i| match i {
                syn::ForeignItem::Fn(f) => {
                    !self.foreign.contains(&f.sig.ident.to_string())
                        || self.used.contains(&f.sig.ident.to_string())
                }
                syn::ForeignItem::Static(s) => {
                    !self.globals.contains_key(&s.ident.to_string())
                        || self.used.contains(&s.ident.to_string())
                }
                _ => true,
            });
        }
    }
    for (_, file) in files.iter_mut() {
        Remove {
            foreign: &foreign,
            globals: &foreign_globals,
            used: &used,
        }
        .visit_file_mut(file);
    }
    if lift {
        let globals = SymbolTable::from_files(files);
        for (path, file) in files.iter_mut() {
            *rw.cur_file.borrow_mut() = Some(path.clone());
            Lifter {
                rw,
                symbols: globals.clone(),
            }
            .visit_file_mut(file);
        }
    }
    rw.cur_file.borrow_mut().take();
}

struct Mapper<'a> {
    rw: &'a Rewriter,
    records: &'a HashMap<String, &'static str>,
    foreign: &'a HashSet<String>,
    symbols: SymbolTable,
    static_init: bool,
    foreign_globals: &'a HashMap<String, Type>,
    lift: bool,
}
impl Mapper<'_> {
    fn char_element(&self, expr: &Expr) -> Type {
        let ty = if let Expr::Index(idx) = expr
            && self.global(&idx.expr, "tzname")
        {
            self.foreign_globals.get("tzname").and_then(|ty| {
                if let Type::Array(a) = resolved(ty, &self.symbols) {
                    Some((*a.elem).clone())
                } else {
                    None
                }
            })
        } else {
            expression_type(expr, &self.symbols)
        };
        ty.and_then(|ty| match resolved(&ty, &self.symbols) {
            Type::Ptr(p) => Some((*p.elem).clone()),
            _ => None,
        })
        .unwrap_or_else(c_char)
    }
    fn tm_type(&self, t: &Type) -> bool {
        let t = resolved(t, &self.symbols);
        match t {
            Type::Reference(r) => self.tm_type(&r.elem), Type::Ptr(p) => self.tm_type(&p.elem),
            _ => last_type(t).is_some_and(|n| self.records.get(&n) == Some(&"Tm") || n == "tm" && matches!(t, Type::Path(p) if p.path.segments.first().is_some_and(|s| s.ident == "libc"))),
        }
    }
    fn zone_field(&self, field: &syn::ExprField) -> bool {
        matches!(&field.member, Member::Named(n) if n == "tm_zone" || n == "__tm_zone")
            && expression_type(&field.base, &self.symbols).is_some_and(|t| self.tm_type(&t))
    }
    fn global(&self, e: &Expr, target: &str) -> bool {
        if let Expr::Path(p) = e {
            let seg = &p.path.segments;
            if seg.len() == 1 && seg[0].ident == target {
                return self.foreign_globals.contains_key(target) && !self.symbols.has(target);
            }
            return seg.len() == 2 && seg[0].ident == "libc" && seg[1].ident == target;
        }
        false
    }
    fn zone(&self, expr: &Expr) -> Expr {
        if null(expr) {
            return parse_quote!(::xj_ctime::TzAbbr::NULL);
        }
        if let Some(view) = self.rw.coerce_byte_view_input(expr, &self.symbols)
            && !self.static_init
        {
            return parse_quote!(::xj_ctime::TzAbbr::new(#view));
        }
        {
            let raw = base(expr);
            parse_quote!(::xj_ctime::TzAbbr::from_raw((#raw).cast::<u8>()))
        }
    }
}
impl VisitMut for Mapper<'_> {
    fn visit_item_mut(&mut self, item: &mut Item) {
        if let Item::Struct(s) = item
            && let Some(target) = self.records.get(&s.ident.to_string())
        {
            let ident = &s.ident;
            let vis = &s.vis;
            let attrs: Vec<_> = s
                .attrs
                .iter()
                .filter(|a| !a.path().is_ident("derive") && !a.path().is_ident("repr"))
                .collect();
            let target = syn::Ident::new(target, ident.span());
            *item = parse_quote!(#(#attrs)* #vis type #ident = ::xj_ctime::#target;);
            self.rw.add_dep("xj_ctime");
            return;
        }
        visit_mut::visit_item_mut(self, item);
    }
    fn visit_type_mut(&mut self, t: &mut Type) {
        if let Type::Path(p) = t
            && p.path.segments.len() == 2
            && p.path.segments[0].ident == "libc"
        {
            let target = match p.path.segments[1].ident.to_string().as_str() {
                "tm" => Some("Tm"),
                "timespec" => Some("Timespec"),
                _ => None,
            };
            if let Some(target) = target {
                let target = syn::Ident::new(target, proc_macro2::Span::call_site());
                *t = parse_quote!(::xj_ctime::#target);
                self.rw.add_dep("xj_ctime");
                return;
            }
        }
        visit_mut::visit_type_mut(self, t);
    }
    fn visit_item_static_mut(&mut self, item: &mut syn::ItemStatic) {
        let prev = std::mem::replace(&mut self.static_init, true);
        visit_mut::visit_item_static_mut(self, item);
        self.static_init = prev;
    }
    fn visit_item_const_mut(&mut self, item: &mut syn::ItemConst) {
        let prev = std::mem::replace(&mut self.static_init, true);
        visit_mut::visit_item_const_mut(self, item);
        self.static_init = prev;
    }
    fn visit_item_fn_mut(&mut self, item: &mut syn::ItemFn) {
        let saved = self.symbols.clone();
        for a in &item.sig.inputs {
            if let Some((n, t)) = crate::binding_from_fn_arg(a) {
                self.symbols.insert(n, t);
            }
        }
        visit_mut::visit_item_fn_mut(self, item);
        self.symbols = saved;
    }
    fn visit_block_mut(&mut self, block: &mut syn::Block) {
        let saved = self.symbols.clone();
        for stmt in &mut block.stmts {
            let binding = if let Stmt::Local(l) = stmt {
                Some(l.clone())
            } else {
                None
            };
            self.visit_stmt_mut(stmt);
            if let Some(local) = binding {
                record_local(&local, &mut self.symbols);
            }
        }
        self.symbols = saved;
    }
    fn visit_macro_mut(&mut self, mac: &mut syn::Macro) {
        use syn::parse::Parser;
        use xj_cfmt_core::{Plan, Requirement};
        let seg = &mac.path.segments;
        let Some(last) = seg.last() else { return };
        let operation = last.ident.to_string();
        let index = if seg.len() == 2
            && matches!(seg[0].ident.to_string().as_str(), "xj_cio" | "xj_cfmt")
        {
            match operation.trim_end_matches("_raw") {
                "printf" => Some(0),
                "fprintf" | "sprintf" => Some(1),
                "snprintf" => Some(2),
                _ => None,
            }
        } else {
            None
        };
        let regular = matches!(
            operation.as_str(),
            "print"
                | "println"
                | "eprint"
                | "eprintln"
                | "format"
                | "format_args"
                | "write"
                | "writeln"
                | "assert"
                | "assert_eq"
                | "assert_ne"
                | "debug_assert"
                | "debug_assert_eq"
                | "debug_assert_ne"
                | "vec"
                | "offset_of"
        );
        if index.is_none() && !regular {
            return;
        }
        if operation == "vec" {
            let tokens = &mac.tokens;
            if let Ok(Expr::Repeat(mut repeat)) = syn::parse2::<Expr>(quote::quote!([#tokens])) {
                self.visit_expr_mut(&mut repeat.expr);
                self.visit_expr_mut(&mut repeat.len);
                let expr = repeat.expr;
                let len = repeat.len;
                mac.tokens = quote::quote!(#expr; #len);
                return;
            }
        }
        let Ok(mut a) = syn::punctuated::Punctuated::<Expr, syn::Token![,]>::parse_terminated
            .parse2(mac.tokens.clone())
        else {
            return;
        };
        if operation == "offset_of" && a.len() == 2 {
            if let Expr::Path(p) = &a[0] {
                let mut ty = Type::Path(syn::TypePath {
                    qself: p.qself.clone(),
                    path: p.path.clone(),
                });
                if self.tm_type(&ty) {
                    if name(&a[1]).as_deref() == Some("__tm_zone") {
                        a[1] = parse_quote!(tm_zone);
                    }
                    if name(&a[1]).as_deref() == Some("__tm_gmtoff") {
                        a[1] = parse_quote!(tm_gmtoff);
                    }
                }
                self.visit_type_mut(&mut ty);
                if let Type::Path(p) = ty {
                    a[0] = Expr::Path(syn::ExprPath {
                        attrs: Vec::new(),
                        qself: p.qself,
                        path: p.path,
                    });
                }
            }
            mac.tokens = a.to_token_stream();
            return;
        }
        let bytes = index
            .and_then(|i| a.get(i))
            .and_then(crate::printf::literal_bytes);
        let requirements = bytes
            .as_ref()
            .and_then(|b| Plan::new(b).ok())
            .map(|p| p.requirements);
        for (i, arg) in a.iter_mut().enumerate() {
            let string = index.is_some_and(|index| {
                i > index
                    && requirements.as_ref().and_then(|r| r.get(i - index - 1))
                        == Some(&Requirement::String)
            });
            if string {
                if let Expr::Field(f) = base(arg)
                    && self.zone_field(f)
                {
                    let mut f = f.clone();
                    f.member = parse_quote!(tm_zone);
                    self.visit_expr_mut(&mut f.base);
                    *arg = self.rw.byte_view_bytes(&parse_quote!(&(#f)));
                    continue;
                }
                if let Expr::Index(idx) = base(arg)
                    && self.global(&idx.expr, "tzname")
                {
                    let index = &idx.index;
                    *arg = self
                        .rw
                        .byte_view_bytes(&parse_quote!(&::xj_ctime::c::tzname()[#index]));
                    continue;
                }
            }
            self.visit_expr_mut(arg);
        }
        mac.tokens = a.to_token_stream();
    }
    fn visit_expr_mut(&mut self, expr: &mut Expr) {
        if let Expr::Index(idx) = expr
            && self.global(&idx.expr, "tzname")
        {
            let mut index = (*idx.index).clone();
            self.visit_expr_mut(&mut index);
            let char_ty = self.char_element(expr);
            *expr = if self.lift {
                parse_quote!(::xj_ctime::c::tzname()[#index].as_ptr().cast_mut().cast::<#char_ty>())
            } else {
                parse_quote!((*::xj_ctime::c_abi::tzname())[ #index ].cast_mut().cast::<#char_ty>())
            };
            self.rw.add_dep("xj_ctime");
            return;
        }
        for target in ["timezone", "daylight", "tzname"] {
            if self.global(expr, target) {
                let ident = syn::Ident::new(target, proc_macro2::Span::call_site());
                *expr = if target == "tzname" {
                    let array_ty = self
                        .foreign_globals
                        .get("tzname")
                        .cloned()
                        .unwrap_or_else(|| parse_quote!([*mut ::core::ffi::c_char; 2]));
                    parse_quote!(*::xj_ctime::c_abi::tzname().cast::<#array_ty>())
                } else {
                    parse_quote!(::xj_ctime::c_abi::#ident())
                };
                self.rw.add_dep("xj_ctime");
                return;
            }
        }
        if let Expr::Struct(s) = expr {
            let original = Type::Path(syn::TypePath {
                qself: s.qself.clone(),
                path: s.path.clone(),
            });
            if self.tm_type(&original) {
                for f in &mut s.fields {
                    if matches!(&f.member, Member::Named(n) if n == "tm_zone" || n == "__tm_zone") {
                        f.member = parse_quote!(tm_zone);
                        f.expr = self.zone(&f.expr);
                        f.colon_token = Some(Default::default());
                    }
                    if matches!(&f.member, Member::Named(n) if n == "__tm_gmtoff") {
                        f.member = parse_quote!(tm_gmtoff);
                    }
                }
            }
            if s.path.segments.len() == 2 && s.path.segments[0].ident == "libc" {
                let target = match s.path.segments[1].ident.to_string().as_str() {
                    "tm" => Some("Tm"),
                    "timespec" => Some("Timespec"),
                    _ => None,
                };
                if let Some(target) = target {
                    let target = syn::Ident::new(target, proc_macro2::Span::call_site());
                    s.path = parse_quote!(::xj_ctime::#target);
                    self.rw.add_dep("xj_ctime");
                }
            }
        }
        if let Expr::Assign(a) = expr
            && let Expr::Field(f) = unparen(&a.left)
            && self.zone_field(f)
        {
            let mut f = f.clone();
            f.member = parse_quote!(tm_zone);
            self.visit_expr_mut(&mut f.base);
            let right = self.zone(&a.right);
            a.left = Box::new(Expr::Field(f));
            a.right = Box::new(right);
            return;
        }
        if let Expr::Field(f) = expr
            && self.zone_field(f)
        {
            let char_ty = self.char_element(&Expr::Field(f.clone()));
            let mut f = f.clone();
            f.member = parse_quote!(tm_zone);
            self.visit_expr_mut(&mut f.base);
            *expr = parse_quote!((#f).as_ptr().cast::<#char_ty>());
            return;
        }
        if let Expr::Field(f) = expr
            && matches!(&f.member, Member::Named(n) if n == "__tm_gmtoff")
            && expression_type(&f.base, &self.symbols).is_some_and(|t| self.tm_type(&t))
        {
            f.member = parse_quote!(tm_gmtoff);
        }
        if let Expr::Call(call) = expr
            && let Expr::Path(p) = &*call.func
        {
            let parts: Vec<_> = p
                .path
                .segments
                .iter()
                .map(|s| s.ident.to_string())
                .collect();
            let n = parts.last().map(String::as_str).unwrap_or("");
            let native = parts.len() == 1 && self.foreign.contains(n) && !self.symbols.has(n)
                || parts.len() == 2 && parts[0] == "libc" && args(n).is_some()
                || parts.len() == 3
                    && parts[0] == "xj_ctime"
                    && parts[1] == "compat"
                    && n == "difftime";
            if native
                && let Some(roles) = args(n)
                && roles.len() == call.args.len()
            {
                let ident = syn::Ident::new(n, p.path.segments.last().unwrap().ident.span());
                let mut call = call.clone();
                // Descend before wrapping byte results, avoiding a second rewrite.
                for (index, a) in call.args.iter_mut().enumerate() {
                    self.visit_expr_mut(a);
                    if matches!(roles[index], Bytes | Buffer) {
                        let raw = base(a);
                        *a = parse_quote!((#raw).cast::<u8>());
                    }
                }
                call.func = Box::new(parse_quote!(::xj_ctime::c_abi::#ident));
                self.rw.add_dep("xj_ctime");
                if matches!(
                    n,
                    "asctime" | "ctime" | "asctime_r" | "ctime_r" | "strptime"
                ) {
                    // New C2Rust output uses u8; older output uses c_char.
                    // Preserve the declaration's element type for its callers.
                    let char_ty = self.char_element(expr);
                    *expr = parse_quote!((#call).cast::<#char_ty>());
                } else {
                    *expr = Expr::Call(call);
                }
                return;
            }
        }
        visit_mut::visit_expr_mut(self, expr);
    }
}

fn null(e: &Expr) -> bool {
    match base(e) {
        Expr::Call(c) if c.args.is_empty() => {
            matches!(&*c.func, Expr::Path(p) if p.path.segments.last().is_some_and(|s| s.ident == "null_mut" || s.ident == "null"))
        }
        Expr::Lit(l) => {
            matches!(&l.lit, syn::Lit::Int(n) if n.base10_parse::<u64>().ok() == Some(0))
        }
        _ => false,
    }
}
fn place(e: &Expr, symbols: &SymbolTable, output: bool) -> bool {
    match unparen(e) {
        Expr::Path(p) => p.path.get_ident().is_some_and(|n| {
            symbols.has(&n.to_string())
                && !symbols.constants.contains(&n.to_string())
                && (!output || !symbols.mutable_statics.contains(&n.to_string()))
        }),
        Expr::Field(f) => place(&f.base, symbols, output),
        Expr::Index(i) => {
            place(&i.expr, symbols, output) && matches!(unparen(&i.index), Expr::Lit(_))
        }
        _ => false,
    }
}
fn borrow(e: &Expr, symbols: &SymbolTable, output: bool) -> Option<Expr> {
    match base(e) {
        Expr::RawAddr(r)
            if (!output || matches!(r.mutability, syn::PointerMutability::Mut(_)))
                && place(&r.expr, symbols, output) =>
        {
            let p = &r.expr;
            Some(if output {
                parse_quote!(&mut #p)
            } else {
                parse_quote!(&#p)
            })
        }
        Expr::Reference(r) if !output || r.mutability.is_some() => Some(Expr::Reference(r.clone())),
        e if crate::printf::expr_type(e, symbols).is_some_and(
            |t| matches!(t, Type::Reference(r) if !output || r.mutability.is_some()),
        ) =>
        {
            Some(e.clone())
        }
        _ => None,
    }
}
struct Lifter<'a> {
    rw: &'a Rewriter,
    symbols: SymbolTable,
}
impl Lifter<'_> {
    fn call(&self, call: &syn::ExprCall) -> Option<Expr> {
        let Expr::Path(p) = &*call.func else {
            return None;
        };
        let seg = &p.path.segments;
        if seg.len() != 3
            || seg[0].ident != "xj_ctime"
            || !(seg[1].ident == "c_abi"
                || seg[1].ident == "c" && seg[2].ident.to_string().ends_with("_raw"))
        {
            return None;
        }
        let n = seg[2].ident.to_string();
        let n = n.trim_end_matches("_raw");
        if matches!(n, "localtime" | "gmtime" | "asctime" | "ctime" | "tzname") {
            return None;
        }
        let roles = args(n)?;
        if roles.len() != call.args.len() {
            return None;
        }
        let mut result = call.clone();
        let mut lifted = vec![false; roles.len()];

        // A safe borrow can conflict with a raw argument naming the same place.
        // Conservatively leave the entire call raw in that case.
        let mut occurrences = HashMap::<String, usize>::new();
        fn paths(e: &Expr, out: &mut HashSet<String>) {
            struct Scan<'a>(&'a mut HashSet<String>);
            impl VisitMut for Scan<'_> {
                fn visit_expr_path_mut(&mut self, p: &mut syn::ExprPath) {
                    if let Some(n) = p.path.get_ident() {
                        self.0.insert(n.to_string());
                    }
                }
            }
            Scan(out).visit_expr_mut(&mut e.clone());
        }
        for (a, role) in call.args.iter().zip(roles) {
            if !matches!(role, Value) {
                let mut names = HashSet::new();
                paths(a, &mut names);
                for name in names {
                    *occurrences.entry(name).or_default() += 1;
                }
            }
        }
        for (i, role) in roles.iter().enumerate() {
            let a = &call.args[i];
            let replacement = match role {
                Value => Some(a.clone()),
                Input => borrow(a, &self.symbols, false),
                Output => borrow(a, &self.symbols, true),
                Optional if null(a) => Some(parse_quote!(None)),
                Optional => borrow(a, &self.symbols, true).map(|b| parse_quote!(Some(#b))).or_else(|| {
                    if name(a).as_deref() == Some("None") || matches!(a, Expr::Call(c) if matches!(&*c.func, Expr::Path(p) if p.path.is_ident("Some"))) { Some(a.clone()) } else { None }
                }),
                Bytes => self.rw.coerce_byte_view_input(a, &self.symbols).map(|e| *e),
                Buffer => None,
            };
            if let Some(replacement) = replacement {
                if !matches!(role, Value) {
                    let mut names = HashSet::new();
                    paths(a, &mut names);
                    if names
                        .iter()
                        .any(|n| occurrences.get(n).copied().unwrap_or(0) > 1)
                    {
                        return None;
                    }
                }
                result.args[i] = replacement;
                lifted[i] = true;
            }
        }

        let ident = syn::Ident::new(n, seg[2].ident.span());
        let buffer_index = roles.iter().position(|a| matches!(a, Buffer));
        if let Some(index) = buffer_index {
            if lifted.iter().enumerate().all(|(i, v)| *v || i == index) {
                if let Some((buffer, pointer)) = self.destination(&call.args[index]) {
                    let (dest, new_args): (Expr, Vec<Expr>) = if n == "strftime" {
                        let size = &call.args[1];
                        let fmt = &result.args[2];
                        let tm = &result.args[3];
                        let dest = parse_quote!(::xj_ctime::CStrBuffer::cstr_dest(#buffer).limit(#size as usize));
                        (dest, vec![fmt.clone(), tm.clone()])
                    } else {
                        (
                            parse_quote!(::xj_ctime::CStrBuffer::cstr_dest(#buffer)),
                            vec![result.args[0].clone()],
                        )
                    };
                    if n == "strftime" {
                        return Some(parse_quote!(::xj_ctime::c::strftime(#dest, #(#new_args),*)));
                    }
                    let input = &new_args[0];
                    return Some(parse_quote!({
                        let xj_time_buffer = #buffer;
                        let xj_time_result = ::xj_ctime::c::#ident(#input, ::xj_ctime::CStrBuffer::cstr_dest(&mut *xj_time_buffer));
                        xj_time_result.map_or(::core::ptr::null_mut(), |_| #pointer)
                    }));
                }
            }
        } else if lifted.iter().all(|v| *v) {
            let a: Vec<_> = result.args.iter().collect();
            if matches!(n, "localtime_r" | "gmtime_r") {
                return Some(
                    parse_quote!(::xj_ctime::c::#ident(#(#a),*).map_or(::core::ptr::null_mut(), |tm| tm as *mut ::xj_ctime::Tm)),
                );
            }
            if n == "strptime" {
                let input = a[0];
                let fmt = a[1];
                let tm = a[2];
                let bytes = self.rw.byte_view_bytes(&parse_quote!(xj_time_input));
                return Some(parse_quote!({
                    let xj_time_input = #input;
                    ::xj_ctime::c::strptime(xj_time_input, #fmt, #tm).map_or(::core::ptr::null_mut(), |offset| {
                        (#bytes).as_ptr().wrapping_add(offset).cast_mut()
                    })
                }));
            }
            return Some(parse_quote!(::xj_ctime::c::#ident(#(#a),*)));
        }
        if matches!(
            n,
            "localtime_r"
                | "gmtime_r"
                | "strftime"
                | "strptime"
                | "asctime_r"
                | "ctime_r"
                | "nanosleep"
                | "clock_nanosleep"
        ) && lifted
            .iter()
            .enumerate()
            .any(|(i, v)| *v && !matches!(roles[i], Value))
        {
            let ident = syn::Ident::new(&format!("{n}_raw"), ident.span());
            let a = &result.args;
            Some(parse_quote!(::xj_ctime::c::#ident(#a)))
        } else {
            None
        }
    }
    fn destination(&self, e: &Expr) -> Option<(Expr, Expr)> {
        let e = base(e);
        let storage = match e {
            Expr::MethodCall(m) if m.method == "as_mut_ptr" && m.args.is_empty() => &*m.receiver,
            Expr::RawAddr(r) => &*r.expr,
            _ => return None,
        };
        if !place(storage, &self.symbols, true) {
            return None;
        }
        let ty = self.symbols.type_of_expr(storage)?;
        let elem = match &ty {
            Type::Array(a) => Some(&*a.elem),
            Type::Reference(r) => match &*r.elem {
                Type::Slice(s) => Some(&*s.elem),
                Type::Array(a) => Some(&*a.elem),
                _ => None,
            },
            _ => None,
        };
        if let Some(elem) = elem {
            let buffer: Expr = if crate::byte_input::is_u8_type(elem) {
                parse_quote!(&mut #storage)
            } else if matches!(last_type(elem).as_deref(), Some("i8" | "c_char")) {
                self.rw.add_dep("xj_cstr");
                parse_quote!(::xj_cstr::ByteSlice::as_mut_u8_slice(&mut #storage[..]))
            } else {
                return None;
            };
            return Some((buffer, parse_quote!(xj_time_buffer.as_mut_ptr())));
        }
        if matches!(last_type(&ty).as_deref(), Some("ZString" | "ZStr")) {
            return Some((
                parse_quote!(&mut #storage),
                parse_quote!(xj_time_buffer.as_buffer_mut().as_mut_ptr()),
            ));
        }
        None
    }
}
impl VisitMut for Lifter<'_> {
    fn visit_expr_mut(&mut self, expr: &mut Expr) {
        // Rewriting children first lets a C-character result cast remain outside
        // the lifted call, preserving pointer consumers during gradual lifting.
        visit_mut::visit_expr_mut(self, expr);
        if let Expr::Call(call) = expr
            && let Some(new) = self.call(call)
        {
            *expr = new;
        }
    }
    fn visit_item_fn_mut(&mut self, item: &mut syn::ItemFn) {
        let saved = self.symbols.clone();
        for a in &item.sig.inputs {
            if let Some((n, t)) = crate::binding_from_fn_arg(a) {
                self.symbols.insert(n, t);
            }
        }
        visit_mut::visit_item_fn_mut(self, item);
        self.symbols = saved;
    }
    fn visit_block_mut(&mut self, block: &mut syn::Block) {
        let saved = self.symbols.clone();
        guide_static_results(block, &self.symbols);
        for stmt in &mut block.stmts {
            self.visit_stmt_mut(stmt);
            if let Stmt::Local(l) = stmt {
                record_local(l, &mut self.symbols);
            }
        }
        self.symbols = saved;
    }
}

fn ctime_call(e: &Expr) -> Option<(&str, &syn::ExprCall)> {
    let Expr::Call(c) = base(e) else { return None };
    let Expr::Path(p) = &*c.func else { return None };
    if p.path.segments.len() != 3
        || p.path.segments[0].ident != "xj_ctime"
        || p.path.segments[1].ident != "c_abi"
    {
        return None;
    }
    let n = &p.path.segments[2].ident;
    let n = if n == "localtime" {
        "localtime"
    } else if n == "gmtime" {
        "gmtime"
    } else if n == "asctime" {
        "asctime"
    } else if n == "ctime" {
        "ctime"
    } else {
        return None;
    };
    Some((n, c))
}
fn mentions(stmt: &Stmt, target: &str) -> bool {
    // Token identifiers include uses inside opaque macros and item bodies.
    fn scan(ts: proc_macro2::TokenStream, target: &str) -> bool {
        ts.into_iter().any(|t| match t {
            proc_macro2::TokenTree::Ident(i) => i == target,
            proc_macro2::TokenTree::Group(g) => scan(g.stream(), target),
            _ => false,
        })
    }
    scan(stmt.to_token_stream(), target)
}
struct StaticUses<'a> {
    target: &'a syn::Ident,
    text: bool,
    valid: bool,
    writes: bool,
}
impl StaticUses<'_> {
    fn is_target(&self, e: &Expr) -> bool {
        name(e).as_deref() == Some(self.target.to_string().as_str())
    }
    fn deref_target(&self, e: &Expr) -> bool {
        matches!(unparen(e), Expr::Unary(u) if matches!(u.op, syn::UnOp::Deref(_)) && self.is_target(&u.expr))
    }
    fn field(&mut self, field: &mut syn::ExprField, output: bool) -> bool {
        if self.text || !self.deref_target(&field.base) {
            return false;
        }
        let p = self.target;
        field.base = Box::new(if output {
            self.writes = true;
            parse_quote!(#p.as_mut().unwrap())
        } else {
            parse_quote!(#p.as_ref().unwrap())
        });
        true
    }
}
impl VisitMut for StaticUses<'_> {
    fn visit_pat_ident_mut(&mut self, pat: &mut syn::PatIdent) {
        if pat.ident == *self.target {
            self.valid = false;
        }
    }
    fn visit_expr_mut(&mut self, e: &mut Expr) {
        let p = self.target;
        if let Expr::MethodCall(m) = e
            && m.method == "is_null"
            && m.args.is_empty()
            && self.is_target(&m.receiver)
        {
            *e = parse_quote!(#p.is_none());
            return;
        }
        if let Expr::Assign(a) = e
            && let Expr::Field(f) = &mut *a.left
            && self.field(f, true)
        {
            self.visit_expr_mut(&mut a.right);
            return;
        }
        if let Expr::Binary(a) = e
            && matches!(
                a.op,
                syn::BinOp::AddAssign(_)
                    | syn::BinOp::SubAssign(_)
                    | syn::BinOp::MulAssign(_)
                    | syn::BinOp::DivAssign(_)
                    | syn::BinOp::RemAssign(_)
            )
            && let Expr::Field(f) = &mut *a.left
            && self.field(f, true)
        {
            self.visit_expr_mut(&mut a.right);
            return;
        }
        if let Expr::Field(f) = e
            && self.field(f, false)
        {
            return;
        }
        if !self.text && self.deref_target(e) {
            *e = parse_quote!(#p.unwrap());
            return;
        }
        if self.is_target(e) {
            self.valid = false;
            return;
        }
        // An unknown callee can overwrite a static buffer indirectly. Require
        // the interval to contain no calls, apart from uses transformed above.
        if matches!(
            e,
            Expr::Call(_)
                | Expr::MethodCall(_)
                | Expr::Closure(_)
                | Expr::Async(_)
                | Expr::Reference(_)
                | Expr::RawAddr(_)
        ) {
            self.valid = false;
        }
        visit_mut::visit_expr_mut(self, e);
    }
    fn visit_macro_mut(&mut self, m: &mut syn::Macro) {
        use syn::parse::Parser;
        let supported = self.text
            && m.path.segments.len() == 2
            && matches!(
                m.path.segments[0].ident.to_string().as_str(),
                "xj_cio" | "xj_cfmt"
            )
            && m.path.segments[1].ident == "printf";
        let Ok(mut a) = syn::punctuated::Punctuated::<Expr, syn::Token![,]>::parse_terminated
            .parse2(m.tokens.clone())
        else {
            self.valid = false;
            return;
        };
        if supported
            && a.len() == 2
            && self.is_target(base(&a[1]))
            && matches!(&a[0], Expr::Lit(l) if matches!(&l.lit, syn::Lit::Str(s) if s.value().matches('%').count() == 1 && s.value().contains("%s")))
        {
            let p = self.target;
            a[1] = parse_quote!(#p.as_ref().unwrap().as_bytes_with_nul());
            m.tokens = a.to_token_stream();
            return;
        }
        // Even macros without the target can invoke a static-result function.
        self.valid = false;
    }
    fn visit_item_mut(&mut self, _: &mut Item) {
        self.valid = false;
    }
}
fn guide_static_results(block: &mut syn::Block, symbols: &SymbolTable) {
    let mut symbols = symbols.clone();
    for i in 0..block.stmts.len() {
        let candidate = (|| {
            let Stmt::Local(l) = &block.stmts[i] else {
                return None;
            };
            let pat = match &l.pat {
                Pat::Type(t) => &*t.pat,
                other => other,
            };
            let Pat::Ident(pat) = pat else { return None };
            if pat.by_ref.is_some() || pat.subpat.is_some() {
                return None;
            }
            let init = l.init.as_ref()?;
            if init.diverge.is_some() {
                return None;
            }
            let (n, call) = ctime_call(&init.expr)?;
            if call.args.len() != 1 {
                return None;
            }
            let input = borrow(&call.args[0], &symbols, false)?;
            let last = (i + 1..block.stmts.len())
                .rfind(|&j| mentions(&block.stmts[j], &pat.ident.to_string()))?;
            Some((pat.clone(), n.to_owned(), input, last))
        })();
        if let Some((mut pat, n, input, last)) = candidate {
            let mut tail = block.stmts[i + 1..=last].to_vec();
            let mut checker = StaticUses {
                target: &pat.ident,
                text: n == "asctime" || n == "ctime",
                valid: true,
                writes: false,
            };
            for s in &mut tail {
                checker.visit_stmt_mut(s);
            }
            if checker.valid {
                if checker.writes {
                    pat.mutability = Some(Default::default());
                }
                let ident = syn::Ident::new(&n, pat.ident.span());
                if let Stmt::Local(l) = &mut block.stmts[i] {
                    l.pat = Pat::Ident(pat);
                    l.init.as_mut().unwrap().expr =
                        Box::new(parse_quote!(::xj_ctime::c::#ident(#input)));
                }
                block.stmts[i + 1..=last].clone_from_slice(&tail);
            }
        }
        if let Stmt::Local(l) = &block.stmts[i] {
            record_local(l, &mut symbols);
        }
    }
}
