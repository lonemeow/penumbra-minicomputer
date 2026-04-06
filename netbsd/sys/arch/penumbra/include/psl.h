/*	$NetBSD$	*/

#ifndef _PENUMBRA_PSL_H_
#define _PENUMBRA_PSL_H_

/*
 * Penumbra processor status (SR) register bit definitions.
 *
 * Hardware SR layout (from penumbra_pkg.sv):
 *   Bits 0-3: N, Z, C, V (condition flags)
 *   Bit 30: I  (interrupt enable: 1=enabled)
 *   Bit 31: S  (supervisor mode: 1=supervisor, 0=user)
 */

#define PSL_N		0x00000001	/* negative */
#define PSL_Z		0x00000002	/* zero */
#define PSL_C		0x00000004	/* carry */
#define PSL_V		0x00000008	/* overflow */

#define PSL_I		0x40000000	/* interrupt enable */
#define PSL_S		0x80000000	/* supervisor mode */

/* Condition flags mask */
#define PSL_FLAGS	(PSL_N | PSL_Z | PSL_C | PSL_V)

/* User mode: not supervisor, interrupts enabled */
#define PSL_USERSET	(PSL_I)
#define PSL_USERCLR	(PSL_S)

/* Is this a user-mode SR value? */
#define USERMODE(sr)	(((sr) & PSL_S) == 0)

#endif /* _PENUMBRA_PSL_H_ */
