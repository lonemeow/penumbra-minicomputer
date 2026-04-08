/*	$NetBSD$	*/

/*
 * Use __attribute__((constructor))/(destructor)) for init/fini
 * since Penumbra uses .init_array/.fini_array.
 */

static void __do_global_ctors_aux(void) __attribute__((__constructor__)) __used;
#ifdef SHARED
static void __do_global_dtors_aux(void) __attribute__((__destructor__)) __used;
#endif
