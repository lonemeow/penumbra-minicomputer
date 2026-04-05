/*	$NetBSD$	*/

#ifndef _PENUMBRA_BUS_DEFS_H_
#define _PENUMBRA_BUS_DEFS_H_

/*
 * Machine-dependent bus_space and bus_dma type definitions.
 * Penumbra uses memory-mapped I/O with word-strided registers.
 */

/*
 * bus_space: simple physical address + size.
 * All Penumbra I/O is memory-mapped (no separate I/O address space).
 */
typedef uint32_t	bus_space_tag_t;
typedef uint32_t	bus_space_handle_t;
typedef uint32_t	bus_size_t;
typedef uint32_t	bus_addr_t;

#define PRIxBUSADDR	PRIx32
#define PRIxBUSSIZE	PRIx32
#define PRIuBUSSIZE	PRIu32

/*
 * bus_dma types.
 */
typedef uint32_t	bus_dma_tag_t;

typedef struct bus_dma_segment {
	bus_addr_t	ds_addr;
	bus_size_t	ds_len;
} bus_dma_segment_t;

typedef struct bus_dmamap {
	bus_size_t	dm_mapsize;
	int		dm_nsegs;
	bus_dma_segment_t *dm_segs;
} *bus_dmamap_t;

/* Flags */
#define BUS_SPACE_MAP_CACHEABLE		0x01
#define BUS_SPACE_MAP_LINEAR		0x02
#define BUS_SPACE_MAP_PREFETCHABLE	0x04

#define BUS_DMA_WAITOK		0x000
#define BUS_DMA_NOWAIT		0x001
#define BUS_DMA_ALLOCNOW	0x002

#endif /* _PENUMBRA_BUS_DEFS_H_ */
