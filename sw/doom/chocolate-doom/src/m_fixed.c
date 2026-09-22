//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Fixed point implementation.
//



#include "stdlib.h"

#include "doomtype.h"
#include "i_system.h"

#include "m_fixed.h"
#include "r_phase.h"




//
// Neither routine is built from here.  FixedMul is a `static inline' in
// m_fixed.h, where a caller can have the multiply without the call.
// FixedDiv lives in m_fixed_penumbra.c: this machine divides 32/32
// only, so the 64-bit division upstream writes here becomes a libcall
// on the renderer's hottest path.  The file stays for the diff against
// upstream; the build skips it (COMMON_EXCL in ../../Makefile).
//

