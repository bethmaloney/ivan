/* Supplied by portmath, not vendored from musl.
 *
 * musl defines these in src/internal/features.h, which portmath does not
 * vendor because nothing else in it applies here:
 *
 *   hidden      an ELF visibility attribute. It matters when the functions
 *               live in a shared libc and must not be interposed. These go
 *               into a static library whose only entry points are the pm_*
 *               ones portmath.h declares, so empty is correct.
 *   weak_alias  gives a libc function its second, __-prefixed public name.
 *               A libc needs that so C programs may define their own `sin`;
 *               portmath has no such obligation, so this only has to compile.
 *
 * CMake force-includes this into every portmath translation unit, which keeps
 * portmath/src/ a pure copy of upstream plus the renames vendor.sh documents.
 */
#ifndef PORTMATH_MUSL_SHIM_H
#define PORTMATH_MUSL_SHIM_H

#define hidden
#define weak_alias(old, new) extern __typeof(old) new

#endif
