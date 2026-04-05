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

#endif /* _KERNEL */

#endif /* _PENUMBRA_BUS_FUNCS_H_ */
