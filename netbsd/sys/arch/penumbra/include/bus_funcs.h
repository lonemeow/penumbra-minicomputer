/*	$NetBSD$	*/

#ifndef _PENUMBRA_BUS_FUNCS_H_
#define _PENUMBRA_BUS_FUNCS_H_

/*
 * Machine-dependent bus_space functions.
 *
 * Penumbra has a single MMIO bus and a trivial integer tag, so the
 * single-value read/write/barrier ops collapse to volatile pointer
 * dereferences with no per-call overhead.  They are defined here as
 * function-like macros so every caller substitutes the body inline
 * (one load/store instead of a function call).
 *
 * MI <sys/bus_proto.h> declares external prototypes for these names;
 * <sys/bus.h> includes that header *before* this one, so the
 * prototypes are parsed normally and our macros take effect at every
 * subsequent call site.  The orphan prototypes never resolve to a
 * symbol because every textual call gets macro-expanded — no linker
 * reference is ever emitted.
 *
 * Map/unmap stay out-of-line — they touch UVM and pmap and aren't
 * on any hot path.  bus_dma_* are declared as panic stubs so MI
 * drivers behind SMC_CAPS_DMA-gated paths still link.
 */

#ifdef _KERNEL

/* Map/unmap (out-of-line, attach-time only) */
int	bus_space_map(bus_space_tag_t, bus_addr_t, bus_size_t,
	    int, bus_space_handle_t *);
void	bus_space_unmap(bus_space_tag_t, bus_space_handle_t, bus_size_t);

/*
 * Read/write single values — volatile pointer dereferences.
 *
 * Each macro argument is referenced exactly once in its expansion so
 * caller-side side effects (e.g. h++ or func()) evaluate as expected.
 * The tag argument is unreferenced — Penumbra's bus_space_tag_t
 * carries no information.
 */
#define	bus_space_read_1(t, h, o)	(*(volatile uint8_t  *)((h) + (o)))
#define	bus_space_read_2(t, h, o)	(*(volatile uint16_t *)((h) + (o)))
#define	bus_space_read_4(t, h, o)	(*(volatile uint32_t *)((h) + (o)))

#define	bus_space_write_1(t, h, o, v)	\
	(*(volatile uint8_t  *)((h) + (o)) = (v))
#define	bus_space_write_2(t, h, o, v)	\
	(*(volatile uint16_t *)((h) + (o)) = (v))
#define	bus_space_write_4(t, h, o, v)	\
	(*(volatile uint32_t *)((h) + (o)) = (v))

/*
 * write_multi: same register, many bytes.  Used by the MI com driver
 * for FIFO drains.  A static inline (under a private name) gives the
 * compiler the loop body to fold into the caller's register
 * allocation; the public macro alias resolves before the
 * <sys/bus_proto.h> prototype is reached at any call site.
 */
static inline void
__penumbra_bus_write_multi_1(bus_space_tag_t t __unused, bus_space_handle_t h,
    bus_size_t o, const uint8_t *a, bus_size_t c)
{
	volatile uint8_t *p = (volatile uint8_t *)(h + o);

	while (c-- > 0)
		*p = *a++;
}
#define	bus_space_write_multi_1(t, h, o, a, c)	\
	__penumbra_bus_write_multi_1((t), (h), (o), (a), (c))

/*
 * Barrier: no-op.  Single CPU, no write buffer between core and the
 * memory bus mux, MMIO pages are uncached, and `volatile` already
 * forbids the compiler from reordering across the access.  Cast args
 * to void so any caller-side expressions still type-check.
 */
#define	bus_space_barrier(t, h, o, l, f)	\
	((void)(t), (void)(h), (void)(o), (void)(l), (void)(f))

/* bus_space_is_equal — tag comparison (tags are trivial integers) */
#define	bus_space_is_equal(t1, t2)	((t1) == (t2))

/*
 * bus_dma — not implemented on Penumbra.  The API surface is
 * provided as panic stubs so MI drivers that reference these
 * symbols inside SMC_CAPS_DMA-gated code paths can link.  Any
 * actual invocation fails hard with a readable panic message
 * rather than corrupting memory.
 */
struct proc;
int	bus_dmamap_create(bus_dma_tag_t, bus_size_t, int, bus_size_t,
	    bus_size_t, int, bus_dmamap_t *);
void	bus_dmamap_destroy(bus_dma_tag_t, bus_dmamap_t);
int	bus_dmamap_load(bus_dma_tag_t, bus_dmamap_t, void *, bus_size_t,
	    struct proc *, int);
void	bus_dmamap_unload(bus_dma_tag_t, bus_dmamap_t);
void	bus_dmamap_sync(bus_dma_tag_t, bus_dmamap_t, bus_addr_t,
	    bus_size_t, int);
int	bus_dmamem_alloc(bus_dma_tag_t, bus_size_t, bus_size_t, bus_size_t,
	    bus_dma_segment_t *, int, int *, int);
void	bus_dmamem_free(bus_dma_tag_t, bus_dma_segment_t *, int);
int	bus_dmamem_map(bus_dma_tag_t, bus_dma_segment_t *, int, size_t,
	    void **, int);
void	bus_dmamem_unmap(bus_dma_tag_t, void *, size_t);

#endif /* _KERNEL */

#endif /* _PENUMBRA_BUS_FUNCS_H_ */
