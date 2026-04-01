#ifndef LIBC_H
#define LIBC_H

unsigned long strlen(const char *s);
int isprint(int c);

/* Convert unsigned value to string in buf (must hold ≥12 chars for decimal,
 * ≥9 for hex).  base is 10 or 16.  Returns pointer into buf where the
 * number starts (digits are written right-to-left). */
char *utoa(unsigned int val, char *buf, int base);

#endif
