/*	$NetBSD$	*/

#include <sys/cdefs.h>
__RCSID("$NetBSD$");

#include "../../common/exec_prot.h"

int
exec_prot_support(void)
{
	return PERPAGE_XP;
}
