/*
 * window.h — the measured window of the Dhrystone harness
 *
 * The window is the stretch Dhrystone itself times: from its Begin_Time
 * reading, just before the loop, to its End_Time reading, just after.
 * The shim's time() serves both readings, so it opens and closes the
 * window; entry.c holds the snapshots and prints them.  Dhrystone's
 * output, which waits on the UART, falls outside.
 */

#ifndef DHRY_WINDOW_H
#define DHRY_WINDOW_H

#include <stdint.h>

/* Read the timer and every counter at the window's start. */
void dhry_window_open(void);

/* Read them again at its end; returns the timer ticks it lasted. */
uint32_t dhry_window_close(void);

#endif /* DHRY_WINDOW_H */
