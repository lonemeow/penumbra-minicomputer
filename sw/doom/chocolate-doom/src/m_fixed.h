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
//	Fixed point arithemtics, implementation.
//


#ifndef __M_FIXED__
#define __M_FIXED__

#include <stdint.h>

#include "r_phase.h"


//
// Fixed point, 32bit as 16.16.
//
#define FRACBITS		16
#define FRACUNIT		(1<<FRACBITS)

typedef int fixed_t;

//
// A widening multiply and a shift, which is less work than calling it:
// out of line both arguments are pinned in the argument registers, the
// result travels back through another, and the caller spills whatever
// it holds across the call.  Inline, the renderer's innermost loops
// keep their values in registers and the multiply lands where the
// allocator wants it.
//
static inline fixed_t FixedMul(fixed_t a, fixed_t b)
{
    R_COUNT_FIXEDMUL();
    return (fixed_t) (((int64_t) a * (int64_t) b) >> FRACBITS);
}

fixed_t FixedDiv	(fixed_t a, fixed_t b);



#endif
