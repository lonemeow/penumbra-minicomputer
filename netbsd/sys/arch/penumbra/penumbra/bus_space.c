/*	$NetBSD$	*/

/*
 * Penumbra bus_space implementation.
 *
 * All I/O is memory-mapped.  bus_space_map() allocates kernel VA
 * via UVM and wires it with pmap_kenter_pa (uncached).
 * Read/write ops are simple volatile pointer dereferences.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>

#include <uvm/uvm_extern.h>

#include <machine/bus_defs.h>
#include <machine/bus_funcs.h>
#include <machine/pmap.h>

int
bus_space_map(bus_space_tag_t t, bus_addr_t bpa, bus_size_t size,
    int flags, bus_space_handle_t *bshp)
{
	vaddr_t va;
	paddr_t pa;
	vsize_t sz;

	pa = trunc_page(bpa);
	sz = round_page(bpa + size) - pa;

	va = uvm_km_alloc(kernel_map, sz, PAGE_SIZE, UVM_KMF_VAONLY);
	if (va == 0)
		return ENOMEM;

	for (vsize_t off = 0; off < sz; off += PAGE_SIZE)
		pmap_kenter_pa(va + off, pa + off,
		    VM_PROT_READ | VM_PROT_WRITE, PMAP_NOCACHE);

	pmap_update(pmap_kernel());

	*bshp = va + (bpa - pa);
	return 0;
}

void
bus_space_unmap(bus_space_tag_t t, bus_space_handle_t bsh, bus_size_t size)
{
	vaddr_t va;
	vsize_t sz;

	va = trunc_page(bsh);
	sz = round_page(bsh + size) - va;

	pmap_kremove(va, sz);
	pmap_update(pmap_kernel());
	uvm_km_free(kernel_map, va, sz, UVM_KMF_VAONLY);
}

uint8_t
bus_space_read_1(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o)
{

	return *(volatile uint8_t *)(h + o);
}

uint16_t
bus_space_read_2(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o)
{

	return *(volatile uint16_t *)(h + o);
}

uint32_t
bus_space_read_4(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o)
{

	return *(volatile uint32_t *)(h + o);
}

void
bus_space_write_1(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o,
    uint8_t v)
{

	*(volatile uint8_t *)(h + o) = v;
}

void
bus_space_write_2(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o,
    uint16_t v)
{

	*(volatile uint16_t *)(h + o) = v;
}

void
bus_space_write_4(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o,
    uint32_t v)
{

	*(volatile uint32_t *)(h + o) = v;
}

void
bus_space_write_multi_1(bus_space_tag_t t, bus_space_handle_t h,
    bus_size_t o, const uint8_t *a, bus_size_t c)
{
	volatile uint8_t *p = (volatile uint8_t *)(h + o);

	while (c-- > 0)
		*p = *a++;
}

void
bus_space_barrier(bus_space_tag_t t, bus_space_handle_t h,
    bus_size_t o, bus_size_t l, int flags)
{

	/* No-op: uniprocessor, memory-mapped, no write buffer */
}
