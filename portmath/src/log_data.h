/* Vendored from musl f21a96538f78fa8e2040831b4209b35f2fb581da -- see portmath/README.md.
   Do not edit: regenerate with portmath/vendor.sh. */
/*
 * Copyright (c) 2018, Arm Limited.
 * SPDX-License-Identifier: MIT
 */
#ifndef _LOG_DATA_H
#define _LOG_DATA_H

#include <features.h>

#define LOG_TABLE_BITS 7
#define LOG_POLY_ORDER 6
#define LOG_POLY1_ORDER 12
extern hidden const struct log_data {
	double ln2hi;
	double ln2lo;
	double poly[LOG_POLY_ORDER - 1]; /* First coefficient is 1.  */
	double poly1[LOG_POLY1_ORDER - 1];
	struct {
		double invc, logc;
	} tab[1 << LOG_TABLE_BITS];
#if !__FP_FAST_FMA
	struct {
		double chi, clo;
	} tab2[1 << LOG_TABLE_BITS];
#endif
} __log_data;

#endif
