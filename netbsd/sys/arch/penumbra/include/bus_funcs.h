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

#endif /* _KERNEL */

#endif /* _PENUMBRA_BUS_FUNCS_H_ */
