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

/*
 * bus_dma — panic stubs.  Penumbra has no DMA engine; the API
 * surface only exists so MI drivers that reference these symbols
 * behind capability flags (e.g. SMC_CAPS_DMA in sdmmc) can link.
 * Any actual invocation is a driver bug: something set a DMA
 * capability without a backing implementation.
 */
int
bus_dmamap_create(bus_dma_tag_t t, bus_size_t size, int nsegs,
    bus_size_t maxsegsz, bus_size_t boundary, int flags, bus_dmamap_t *dmamp)
{

	panic("%s: Penumbra has no DMA support", __func__);
}

void
bus_dmamap_destroy(bus_dma_tag_t t, bus_dmamap_t dmam)
{

	panic("%s: Penumbra has no DMA support", __func__);
}

int
bus_dmamap_load(bus_dma_tag_t t, bus_dmamap_t dmam, void *buf,
    bus_size_t buflen, struct proc *p, int flags)
{

	panic("%s: Penumbra has no DMA support", __func__);
}

void
bus_dmamap_unload(bus_dma_tag_t t, bus_dmamap_t dmam)
{

	panic("%s: Penumbra has no DMA support", __func__);
}

void
bus_dmamap_sync(bus_dma_tag_t t, bus_dmamap_t dmam, bus_addr_t offset,
    bus_size_t len, int ops)
{

	panic("%s: Penumbra has no DMA support", __func__);
}

int
bus_dmamem_alloc(bus_dma_tag_t t, bus_size_t size, bus_size_t alignment,
    bus_size_t boundary, bus_dma_segment_t *segs, int nsegs, int *rsegs,
    int flags)
{

	panic("%s: Penumbra has no DMA support", __func__);
}

void
bus_dmamem_free(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs)
{

	panic("%s: Penumbra has no DMA support", __func__);
}

int
bus_dmamem_map(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs,
    size_t size, void **kvap, int flags)
{

	panic("%s: Penumbra has no DMA support", __func__);
}

void
bus_dmamem_unmap(bus_dma_tag_t t, void *kva, size_t size)
{

	panic("%s: Penumbra has no DMA support", __func__);
}
