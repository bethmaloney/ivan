#!/bin/sh
# Regenerate portmath/src/ from upstream musl.
#
# The vendored files are committed, so you only need this to move to a newer
# musl or to audit that what is committed really is what upstream says. Run it
# and `git diff` -- a clean diff means the tree matches upstream at MUSL_COMMIT
# plus exactly the transformations below.
#
# Why musl and not something hand-written: Emscripten's libm *is* musl, so
# vendoring musl makes the WASM build agree with the vendored copy by
# construction rather than by luck. See portmath/README.md.
set -eu

MUSL_URL=https://git.musl-libc.org/git/musl
MUSL_COMMIT=f21a96538f78fa8e2040831b4209b35f2fb581da   # master, 2026-07-28

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/src
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "fetching musl $MUSL_COMMIT"
git init -q "$TMP/musl"
git -C "$TMP/musl" remote add origin "$MUSL_URL"
git -C "$TMP/musl" fetch -q --depth 1 origin "$MUSL_COMMIT"
git -C "$TMP/musl" checkout -q FETCH_HEAD
M=$TMP/musl

rm -rf "$SRC"
mkdir -p "$SRC"

# The entry points the game needs, plus everything they reach. sin/cos/sincos
# pull in the argument reduction and the kernels; log/log10/pow/exp pull in
# their lookup tables; all of them can reach the error-handling helpers.
ENTRIES="sin cos sincos log log10 pow atan hypot exp"
SUPPORT="__sin __cos __rem_pio2 __rem_pio2_large
         log_data log2_data pow_data exp_data
         __math_oflow __math_uflow __math_xflow __math_invalid __math_divzero"

for f in $ENTRIES $SUPPORT; do
  cp "$M/src/math/$f.c" "$SRC/"
done
for h in exp_data.h log_data.h log2_data.h pow_data.h; do
  cp "$M/src/math/$h" "$SRC/"
done
cp "$M/src/internal/libm.h" "$SRC/"
cp "$M/arch/generic/fp_arch.h" "$SRC/"
cp "$M/COPYRIGHT" "$HERE/COPYRIGHT"

# --- transformation 1: rename the public entry points -----------------------
# Only the entry points, and only where they are defined. musl's internal
# names (__sin, __rem_pio2, the *_data tables) are not exported by glibc, so
# they cannot collide and are left alone -- which keeps the diff against
# upstream small enough to audit by eye.
#
# Anchored to the definition line on purpose. sincos.c names its own output
# parameters `sin` and `cos`, so a blanket rename would corrupt it.
sed -i 's/^double sin(double x)$/double pm_sin(double x)/'                    "$SRC/sin.c"
sed -i 's/^double cos(double x)$/double pm_cos(double x)/'                    "$SRC/cos.c"
sed -i 's/^void sincos(double x, double \*sin, double \*cos)$/void pm_sincos(double x, double *sin, double *cos)/' "$SRC/sincos.c"
sed -i 's/^double log(double x)$/double pm_log(double x)/'                    "$SRC/log.c"
sed -i 's/^double log10(double x)$/double pm_log10(double x)/'                "$SRC/log10.c"
sed -i 's/^double pow(double x, double y)$/double pm_pow(double x, double y)/' "$SRC/pow.c"
sed -i 's/^double atan(double x)$/double pm_atan(double x)/'                  "$SRC/atan.c"
sed -i 's/^double hypot(double x, double y)$/double pm_hypot(double x, double y)/' "$SRC/hypot.c"
sed -i 's/^double exp(double x)$/double pm_exp(double x)/'                    "$SRC/exp.c"

for f in $ENTRIES; do
  grep -q "^[a-z]* pm_$f(" "$SRC/$f.c" || { echo "rename failed: $f" >&2; exit 1; }
done

# --- transformation 1b: rename the two kernels that collide ------------------
# glibc's math.h declares a __-prefixed alias for every libm function it
# exports (__MATHCALL_VEC), so its __sin is double(double) while musl's kernel
# of the same name is double(double,double,int). Renaming musl's is the only
# way to include both headers in one translation unit. They are unambiguous
# identifiers -- no local variable or parameter shares either name -- so a
# word-boundary rename across the subset is safe. __rem_pio2 and the *_data
# tables need no such treatment: glibc exports nothing by those names.
sed -i 's/\b__sin\b/pm_k_sin/g; s/\b__cos\b/pm_k_cos/g' "$SRC"/*.c "$SRC"/libm.h
mv "$SRC/__sin.c" "$SRC/pm_k_sin.c"
mv "$SRC/__cos.c" "$SRC/pm_k_cos.c"

# --- transformation 2: trim libm.h ------------------------------------------
# libm.h declares the whole of musl's internal math surface. Two of those
# declarations conflict with glibc's math.h, which these files also include:
# __tan and __tanl are declared here with musl's argument lists and by glibc
# with its own. __signgam collides outright. The rest are simply unreachable
# from the functions vendored here, so dropping them keeps the header honest
# about what this subset actually provides.
sed -i \
  -e '/^hidden double __tan(/d' \
  -e '/^hidden double __expo2(/d' \
  -e '/^hidden int    __rem_pio2f(/d' \
  -e '/^hidden float  __sindf(/d' \
  -e '/^hidden float  __cosdf(/d' \
  -e '/^hidden float  __tandf(/d' \
  -e '/^hidden float  __expo2f(/d' \
  -e '/^hidden int __rem_pio2l(/d' \
  -e '/^hidden long double __sinl(/d' \
  -e '/^hidden long double __cosl(/d' \
  -e '/^hidden long double __tanl(/d' \
  -e '/^hidden long double __polevll(/d' \
  -e '/^hidden long double __p1evll(/d' \
  -e '/^extern int __signgam;/d' \
  -e '/^hidden double __lgamma_r(/d' \
  -e '/^hidden float __lgammaf_r(/d' \
  -e '/^hidden float __math_xflowf(/d' \
  -e '/^hidden float __math_uflowf(/d' \
  -e '/^hidden float __math_oflowf(/d' \
  -e '/^hidden float __math_divzerof(/d' \
  -e '/^hidden float __math_invalidf(/d' \
  -e '/^hidden long double __math_invalidl(/d' \
  "$SRC/libm.h"

# musl's `hidden` and `weak_alias` come from src/internal/features.h, which is
# not vendored. Rather than patch every file that uses them, portmath/musl_shim.h
# supplies them and CMake force-includes it -- so src/ stays pure upstream plus
# the renames above, and everything of ours lives outside it.

for f in "$SRC"/*.c "$SRC"/*.h; do
  printf '/* Vendored from musl %s -- see portmath/README.md.\n   Do not edit: regenerate with portmath/vendor.sh. */\n' \
         "$MUSL_COMMIT" | cat - "$f" > "$f.tmp" && mv "$f.tmp" "$f"
done

echo "vendored $(ls "$SRC" | wc -l) files into portmath/src"
