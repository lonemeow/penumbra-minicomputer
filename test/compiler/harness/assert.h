// test/compiler/harness/assert.h
#ifndef ASSERT_H
#define ASSERT_H

extern void abort(void);

// NDEBUG gives a no-op assert; otherwise abort() on false.  No
// diagnostic, since this harness has no stderr and exit(127) from
// abort() is clear enough as a failure signal.
#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) ((expr) ? (void)0 : abort())
#endif

#endif
