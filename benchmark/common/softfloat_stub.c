/*
 * softfloat_stub.c — Stub soft-float builtins for bare-metal benchmarks
 *
 * Penumbra has no FPU and no compiler-rt for the bare-metal target.
 * Dhrystone uses float/double only for result reporting AFTER the
 * measurement loop, so these stubs don't affect benchmark accuracy.
 * The harness prints correct timing from the hardware timer.
 *
 * TODO: Replace with proper compiler-rt build for penumbra-unknown-none.
 */

/* Double-precision arithmetic */
double __adddf3(double a, double b)  { (void)a; (void)b; return 0.0; }
double __subdf3(double a, double b)  { (void)a; (void)b; return 0.0; }
double __muldf3(double a, double b)  { (void)a; (void)b; return 0.0; }
double __divdf3(double a, double b)  { (void)a; (void)b; return 0.0; }

/* Single-precision arithmetic */
float __addsf3(float a, float b)     { (void)a; (void)b; return 0.0f; }
float __subsf3(float a, float b)     { (void)a; (void)b; return 0.0f; }
float __mulsf3(float a, float b)     { (void)a; (void)b; return 0.0f; }
float __divsf3(float a, float b)     { (void)a; (void)b; return 0.0f; }

/* Comparisons (return int: negative/zero/positive) */
int __ltdf2(double a, double b)      { (void)a; (void)b; return 0; }
int __ledf2(double a, double b)      { (void)a; (void)b; return 0; }
int __gtdf2(double a, double b)      { (void)a; (void)b; return 0; }
int __gedf2(double a, double b)      { (void)a; (void)b; return 0; }

/* Conversions */
unsigned int __fixunsdfsi(double a)       { (void)a; return 0; }
unsigned int __fixunssfsi(float a)        { (void)a; return 0; }
double __floatunsidf(unsigned int a)      { (void)a; return 0.0; }
float  __floatunsisf(unsigned int a)      { (void)a; return 0.0f; }
double __floatsidf(int a)                 { (void)a; return 0.0; }
float  __floatsisf(int a)                 { (void)a; return 0.0f; }
double __extendsfdf2(float a)             { (void)a; return 0.0; }
float  __truncdfsf2(double a)             { (void)a; return 0.0f; }
