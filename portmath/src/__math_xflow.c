/* Vendored from musl f21a96538f78fa8e2040831b4209b35f2fb581da -- see portmath/README.md.
   Do not edit: regenerate with portmath/vendor.sh. */
#include "libm.h"

double __math_xflow(uint32_t sign, double y)
{
	return eval_as_double(fp_barrier(sign ? -y : y) * y);
}
