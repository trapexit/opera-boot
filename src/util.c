/*
 * opera-boot (GPL-2.0-only): clean-room minimal 3DO bootstrapper.
 *
 * util.c: clib-free helpers shared by the launch link and the dipir blob.
 */
#include "opera-boot.h"

/* 32/32 divide: classic shift-subtract, 32 iterations. */
u32 ob_udiv(u32 n, u32 d)
{
	u32 q = 0;
	u32 r = 0;
	int i;

	if (d == 0)
		return 0;
	for (i = 0; i < 32; i++) {
		r = (r << 1) | (n >> 31);
		n <<= 1;
		q <<= 1;
		if (r >= d) {
			r -= d;
			q |= 1;
		}
	}
	return q;
}
