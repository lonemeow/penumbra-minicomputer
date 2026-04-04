/*
 * mulsi3.c — Software 32-bit multiply for Penumbra (no hardware MUL).
 *
 * Provides __mulsi3 as required by compiler-rt / libgcc when the
 * target lacks a multiply instruction.  Uses shift-and-add.
 *
 * The algorithm is the standard binary long multiplication: iterate
 * over bits of one operand, conditionally adding the other (shifted)
 * to an accumulator.  Works for signed multiply too since the low
 * 32 bits of the product are identical for signed and unsigned.
 */

unsigned int
__mulsi3(unsigned int a, unsigned int b)
{
    unsigned int result = 0;
    while (a)
    {
        if (a & 1)
            result += b;
        a >>= 1;
        b <<= 1;
    }
    return result;
}
