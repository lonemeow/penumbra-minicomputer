/*	$NetBSD$	*/

#ifndef _PENUMBRA_PSL_H_
#define _PENUMBRA_PSL_H_

/*
 * Penumbra processor status (SR) register bit definitions.
 *
 * SR layout (from architecture-overview.md):
 *   Bit 0: S  (supervisor mode: 1=supervisor, 0=user)
 *   Bit 1: I  (interrupt enable: 1=enabled)
 *   Bit 2: M  (MMU enable: 1=enabled)
 *   Bits 28-31: N, Z, C, V (condition flags)
 */

#define PSL_S		0x00000001	/* supervisor mode */
#define PSL_I		0x00000002	/* interrupt enable */
#define PSL_M		0x00000004	/* MMU enable */

#define PSL_N		0x10000000	/* negative */
#define PSL_Z		0x20000000	/* zero */
#define PSL_C		0x40000000	/* carry */
#define PSL_V		0x80000000	/* overflow */

/* Condition flags mask */
#define PSL_FLAGS	(PSL_N | PSL_Z | PSL_C | PSL_V)

/* User mode: not supervisor, interrupts enabled, MMU on */
#define PSL_USERSET	(PSL_I | PSL_M)
#define PSL_USERCLR	(PSL_S)

/* Is this a user-mode SR value? */
#define USERMODE(sr)	(((sr) & PSL_S) == 0)

#endif /* _PENUMBRA_PSL_H_ */
