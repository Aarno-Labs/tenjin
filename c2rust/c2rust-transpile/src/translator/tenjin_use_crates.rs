use super::tenjin::*;
use super::*;

#[allow(clippy::borrowed_box)]
pub fn libz_rs_sys_call_form_cases(t: &Translation, func: &Expr) -> Option<RecognizedCallForm> {
    fn libz_rs_sys_fn_pred(ident: &str) -> bool {
        // See https://docs.rs/libz-sys/latest/libz_sys/
        matches!(
            ident,
            "adler32"
                | "adler32_combine"
                | "compress"
                | "compress2"
                | "compressBound"
                | "crc32"
                | "crc32_z"
                | "crc32_combine"
                | "deflate"
                | "deflateBound"
                | "deflateCopy"
                | "deflateEnd"
                | "deflateInit2_"
                | "deflateInit_"
                | "deflateParams"
                | "deflatePrime"
                | "deflateReset"
                | "deflateSetDictionary"
                | "deflateSetHeader"
                | "deflateTune"
                | "gzclearerr"
                | "gzclose"
                | "gzdirect"
                | "gzdopen"
                | "gzeof"
                | "gzerror"
                | "gzflush"
                | "gzgetc"
                | "gzgets"
                | "gzopen"
                | "gzputc"
                | "gzputs"
                | "gzread"
                | "gzrewind"
                | "gzseek"
                | "gzsetparams"
                | "gztell"
                | "gzungetc"
                | "gzwrite"
                | "inflate"
                | "inflateBack"
                | "inflateBackEnd"
                | "inflateBackInit_"
                | "inflateCopy"
                | "inflateEnd"
                | "inflateGetHeader"
                | "inflateInit2_"
                | "inflateInit_"
                | "inflateMark"
                | "inflatePrime"
                | "inflateReset"
                | "inflateReset2"
                | "inflateSetDictionary"
                | "inflateSync"
                | "uncompress"
                | "zlibCompileFlags"
                | "zlibVersion"
        )
    }

    if t.parsed_guidance
        .borrow()
        .using_crates
        .contains("libz-rs-sys")
    {
        if let Some(ident) = expr_get_ident(func) {
            if libz_rs_sys_fn_pred(&ident) {
                t.use_crate(ExternCrate::LibzRsSys);
                return Some(RecognizedCallForm::RetargetedCallee(
                    mk().path_expr(vec!["libz_rs_sys", &ident]),
                ));
            }
        }
    }
    None
}

/// The `bz_stream`-based and buffer-to-buffer entry points of libbz2, whose
/// libbz2-rs-sys signatures line up with what Tenjin translates the C
/// declarations to, so calls can be retargeted directly.
/// See https://docs.rs/libbz2-rs-sys/latest/libbz2_rs_sys/
fn libbz2_rs_sys_direct_fn_pred(ident: &str) -> bool {
    matches!(
        ident,
        "BZ2_bzBuffToBuffCompress"
            | "BZ2_bzBuffToBuffDecompress"
            | "BZ2_bzCompress"
            | "BZ2_bzCompressEnd"
            | "BZ2_bzCompressInit"
            | "BZ2_bzDecompress"
            | "BZ2_bzDecompressEnd"
            | "BZ2_bzDecompressInit"
            | "BZ2_bzlibVersion"
    )
}

/// The remaining libbz2 entry points are the `BZFILE`-based ones. bzlib.h
/// declares `typedef void BZFILE;`, so Tenjin translates `BZFILE*` as
/// `*mut c_void`, whereas libbz2-rs-sys has its own opaque `BZFILE` struct;
/// likewise `FILE*` translates to a pointer to whichever `FILE` struct the
/// platform headers produced, not to `libc::FILE`. So instead of retargeting
/// these calls directly, we route them through a shim that casts the pointers
/// across. The `FILE` parameters are left generic, since the name of the
/// translated `FILE` struct varies with the platform (and may be
/// disambiguated with a unique suffix).
///
/// Returns the shim's definition; its name is always `xj_` plus `ident`.
fn libbz2_rs_sys_shim_defn(ident: &str) -> Option<&'static str> {
    Some(match ident {
        "BZ2_bzReadOpen" => {
            "unsafe fn xj_BZ2_bzReadOpen<F>(bzerror: *mut ::core::ffi::c_int, f: *mut F, verbosity: ::core::ffi::c_int, small: ::core::ffi::c_int, unused: *mut ::core::ffi::c_void, nUnused: ::core::ffi::c_int) -> *mut ::core::ffi::c_void { libbz2_rs_sys::BZ2_bzReadOpen(bzerror, f.cast(), verbosity, small, unused, nUnused).cast() }"
        }
        "BZ2_bzReadClose" => {
            "unsafe fn xj_BZ2_bzReadClose(bzerror: *mut ::core::ffi::c_int, b: *mut ::core::ffi::c_void) { libbz2_rs_sys::BZ2_bzReadClose(bzerror, b.cast()) }"
        }
        "BZ2_bzReadGetUnused" => {
            "unsafe fn xj_BZ2_bzReadGetUnused(bzerror: *mut ::core::ffi::c_int, b: *mut ::core::ffi::c_void, unused: *mut *mut ::core::ffi::c_void, nUnused: *mut ::core::ffi::c_int) { libbz2_rs_sys::BZ2_bzReadGetUnused(bzerror, b.cast(), unused, nUnused) }"
        }
        "BZ2_bzRead" => {
            "unsafe fn xj_BZ2_bzRead(bzerror: *mut ::core::ffi::c_int, b: *mut ::core::ffi::c_void, buf: *mut ::core::ffi::c_void, len: ::core::ffi::c_int) -> ::core::ffi::c_int { libbz2_rs_sys::BZ2_bzRead(bzerror, b.cast(), buf, len) }"
        }
        "BZ2_bzWriteOpen" => {
            "unsafe fn xj_BZ2_bzWriteOpen<F>(bzerror: *mut ::core::ffi::c_int, f: *mut F, blockSize100k: ::core::ffi::c_int, verbosity: ::core::ffi::c_int, workFactor: ::core::ffi::c_int) -> *mut ::core::ffi::c_void { libbz2_rs_sys::BZ2_bzWriteOpen(bzerror, f.cast(), blockSize100k, verbosity, workFactor).cast() }"
        }
        "BZ2_bzWrite" => {
            "unsafe fn xj_BZ2_bzWrite(bzerror: *mut ::core::ffi::c_int, b: *mut ::core::ffi::c_void, buf: *const ::core::ffi::c_void, len: ::core::ffi::c_int) { libbz2_rs_sys::BZ2_bzWrite(bzerror, b.cast(), buf, len) }"
        }
        "BZ2_bzWriteClose" => {
            "unsafe fn xj_BZ2_bzWriteClose(bzerror: *mut ::core::ffi::c_int, b: *mut ::core::ffi::c_void, abandon: ::core::ffi::c_int, nbytes_in: *mut ::core::ffi::c_uint, nbytes_out: *mut ::core::ffi::c_uint) { libbz2_rs_sys::BZ2_bzWriteClose(bzerror, b.cast(), abandon, nbytes_in, nbytes_out) }"
        }
        "BZ2_bzWriteClose64" => {
            "unsafe fn xj_BZ2_bzWriteClose64(bzerror: *mut ::core::ffi::c_int, b: *mut ::core::ffi::c_void, abandon: ::core::ffi::c_int, nbytes_in_lo32: *mut ::core::ffi::c_uint, nbytes_in_hi32: *mut ::core::ffi::c_uint, nbytes_out_lo32: *mut ::core::ffi::c_uint, nbytes_out_hi32: *mut ::core::ffi::c_uint) { libbz2_rs_sys::BZ2_bzWriteClose64(bzerror, b.cast(), abandon, nbytes_in_lo32, nbytes_in_hi32, nbytes_out_lo32, nbytes_out_hi32) }"
        }
        "BZ2_bzopen" => {
            "unsafe fn xj_BZ2_bzopen(path: *const ::core::ffi::c_char, mode: *const ::core::ffi::c_char) -> *mut ::core::ffi::c_void { libbz2_rs_sys::BZ2_bzopen(path, mode).cast() }"
        }
        "BZ2_bzdopen" => {
            "unsafe fn xj_BZ2_bzdopen(fd: ::core::ffi::c_int, mode: *const ::core::ffi::c_char) -> *mut ::core::ffi::c_void { libbz2_rs_sys::BZ2_bzdopen(fd, mode).cast() }"
        }
        "BZ2_bzread" => {
            "unsafe fn xj_BZ2_bzread(b: *mut ::core::ffi::c_void, buf: *mut ::core::ffi::c_void, len: ::core::ffi::c_int) -> ::core::ffi::c_int { libbz2_rs_sys::BZ2_bzread(b.cast(), buf, len) }"
        }
        "BZ2_bzwrite" => {
            "unsafe fn xj_BZ2_bzwrite(b: *mut ::core::ffi::c_void, buf: *const ::core::ffi::c_void, len: ::core::ffi::c_int) -> ::core::ffi::c_int { libbz2_rs_sys::BZ2_bzwrite(b.cast(), buf, len) }"
        }
        "BZ2_bzflush" => {
            "unsafe fn xj_BZ2_bzflush(b: *mut ::core::ffi::c_void) -> ::core::ffi::c_int { libbz2_rs_sys::BZ2_bzflush(b.cast()) }"
        }
        "BZ2_bzclose" => {
            "unsafe fn xj_BZ2_bzclose(b: *mut ::core::ffi::c_void) { libbz2_rs_sys::BZ2_bzclose(b.cast()) }"
        }
        "BZ2_bzerror" => {
            "unsafe fn xj_BZ2_bzerror(b: *const ::core::ffi::c_void, errnum: *mut ::core::ffi::c_int) -> *const ::core::ffi::c_char { libbz2_rs_sys::BZ2_bzerror(b.cast(), errnum) }"
        }
        _ => return None,
    })
}

#[allow(clippy::borrowed_box)]
pub fn libbz2_rs_sys_call_form_cases(t: &Translation, func: &Expr) -> Option<RecognizedCallForm> {
    if !t
        .parsed_guidance
        .borrow()
        .using_crates
        .contains("libbz2-rs-sys")
    {
        return None;
    }

    let ident = expr_get_ident(func)?;

    if libbz2_rs_sys_direct_fn_pred(&ident) {
        t.use_crate(ExternCrate::Libbz2RsSys);
        return Some(RecognizedCallForm::RetargetedCallee(
            mk().path_expr(vec!["libbz2_rs_sys", &ident]),
        ));
    }

    if let Some(shim_defn) = libbz2_rs_sys_shim_defn(&ident) {
        t.use_crate(ExternCrate::Libbz2RsSys);
        t.with_cur_file_item_store(|item_store| {
            item_store.add_item_str_once(shim_defn);
        });
        return Some(RecognizedCallForm::RetargetedCallee(
            mk().path_expr(vec![format!("xj_{ident}")]),
        ));
    }

    None
}
