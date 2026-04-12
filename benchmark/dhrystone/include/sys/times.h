/* sys/times.h — stub for Dhrystone bare-metal build
 *
 * Dhrystone uses this under #ifdef TIMES, but we compile with
 * -DTIME to select the simpler time() path instead.  This stub
 * exists only to satisfy the #include if TIMES is somehow defined.
 */
#ifndef _SHIM_SYS_TIMES_H
#define _SHIM_SYS_TIMES_H

struct tms {
    long tms_utime;
    long tms_stime;
    long tms_cutime;
    long tms_cstime;
};

extern int times(struct tms *buf);

#endif
