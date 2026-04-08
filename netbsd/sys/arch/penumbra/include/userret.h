/*	$NetBSD$	*/

#ifndef _PENUMBRA_USERRET_H_
#define _PENUMBRA_USERRET_H_

#include <sys/userret.h>

static __inline void
userret(struct lwp *l)
{

	mi_userret(l);
}

#endif /* _PENUMBRA_USERRET_H_ */
