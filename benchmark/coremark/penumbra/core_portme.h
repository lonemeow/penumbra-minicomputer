/*
Copyright 2018 Embedded Microprocessor Benchmark Consortium (EEMBC)

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

Original Author: Shay Gal-on

This file has been changed from EEMBC's barebones/core_portme.h: it is
the Penumbra bare-metal port's configuration.
*/
#ifndef CORE_PORTME_H
#define CORE_PORTME_H

#include <stddef.h>

/* Seconds are whole numbers and the score CoreMark prints is truncated
 * to match, so the harness reports its own from the timer and the cycle
 * counter (entry.c).  No floating point reaches the link. */
#define HAS_FLOAT  0
#define HAS_TIME_H 0
#define USE_CLOCK  0
#define HAS_STDIO  0
#define HAS_PRINTF 0

#define _CM_STR(x)  #x
#define _CM_XSTR(x) _CM_STR(x)
#define COMPILER_VERSION                                         \
    "Clang " _CM_XSTR(__clang_major__) "." _CM_XSTR(__clang_minor__) \
    "." _CM_XSTR(__clang_patchlevel__)

/* FLAGS_STR comes from the Makefile, which is where the flags are. */
#define COMPILER_FLAGS FLAGS_STR

/* Code and data are both in main memory, the data block a static array.
 * What that memory is belongs to the machine, so the harness adds it. */
#define MEM_LOCATION "STATIC"

typedef signed short   ee_s16;
typedef unsigned short ee_u16;
typedef signed int     ee_s32;
typedef double         ee_f32;
typedef unsigned char  ee_u8;
typedef unsigned int   ee_u32;
typedef ee_u32         ee_ptr_int;
typedef size_t         ee_size_t;

#define align_mem(x) (void *)(4 + (((ee_ptr_int)(x)-1) & ~3))

/* Timer ticks, at the rate TMFREQ reports. */
typedef ee_u32 CORE_TICKS;

/* The seeds and the iteration count are variables the harness sets
 * before each pass (port.h), which is how one image runs both seed
 * sets and sizes its own run. */
#define SEED_METHOD SEED_VOLATILE
#define MEM_METHOD  MEM_STATIC

#define MULTITHREAD 1
#define USE_PTHREAD 0
#define USE_FORK    0
#define USE_SOCKET  0

/* The harness owns the real entry point and calls CoreMark's main()
 * once per pass, under this name. */
#define MAIN_HAS_NOARGC   1
#define MAIN_HAS_NORETURN 0
#define main              coremark_main

extern ee_u32 default_num_contexts;

typedef struct CORE_PORTABLE_S
{
    ee_u8 portable_id;
} core_portable;

void portable_init(core_portable *p, int *argc, char *argv[]);
void portable_fini(core_portable *p);

int ee_printf(const char *fmt, ...);

/* Where ee_printf.c sends each character. */
void coremark_console_putchar(char c);

#endif /* CORE_PORTME_H */
