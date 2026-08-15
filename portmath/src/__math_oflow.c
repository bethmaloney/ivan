/* Vendored from musl f21a96538f78fa8e2040831b4209b35f2fb581da -- see portmath/README.md.
   Do not edit: regenerate with portmath/vendor.sh. */
#include "libm.h"

double __math_oflow(uint32_t sign)
{
	return __math_xflow(sign, 0x1p769);
}
