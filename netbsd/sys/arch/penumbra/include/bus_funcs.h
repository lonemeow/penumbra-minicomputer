/*	$NetBSD$	*/

#ifndef _PENUMBRA_BUS_FUNCS_H_
#define _PENUMBRA_BUS_FUNCS_H_

/*
 * Machine-dependent bus_space function prototypes.
 * Penumbra uses memory-mapped I/O — bus_space ops are
 * simple volatile pointer reads/writes.
 */

#ifdef _KERNEL

/* Map/unmap */
int	bus_space_map(bus_space_tag_t, bus_addr_t, bus_size_t,
	    int, bus_space_handle_t *);
void	bus_space_unmap(bus_space_tag_t, bus_space_handle_t, bus_size_t);

/* Read/write single values */
uint8_t	 bus_space_read_1(bus_space_tag_t, bus_space_handle_t, bus_size_t);
uint16_t bus_space_read_2(bus_space_tag_t, bus_space_handle_t, bus_size_t);
uint32_t bus_space_read_4(bus_space_tag_t, bus_space_handle_t, bus_size_t);

void	bus_space_write_1(bus_space_tag_t, bus_space_handle_t, bus_size_t, uint8_t);
void	bus_space_write_2(bus_space_tag_t, bus_space_handle_t, bus_size_t, uint16_t);
void	bus_space_write_4(bus_space_tag_t, bus_space_handle_t, bus_size_t, uint32_t);

/* Multi write (used by MI com driver for FIFO drain) */
void	bus_space_write_multi_1(bus_space_tag_t, bus_space_handle_t,
	    bus_size_t, const uint8_t *, bus_size_t);

/* Barrier (no-op on uniprocessor MMIO) */
void	bus_space_barrier(bus_space_tag_t, bus_space_handle_t,
	    bus_size_t, bus_size_t, int);

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
