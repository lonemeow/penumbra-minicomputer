/*
 * The checks in mul-const.c, tuned for penumbra1 (see test-flags.txt), whose
 * ALU operations are cheap enough against its multiply that it decomposes
 * constants the default tuning leaves to the hardware multiply.
 */

#include "mul-const.c"
