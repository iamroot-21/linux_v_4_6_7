/*
 * Virtual Memory Map support
 *
 * (C) 2007 sgi. Christoph Lameter.
 *
 * Virtual memory maps allow VM primitives pfn_to_page, page_to_pfn,
 * virt_to_page, page_address() to be implemented as a base offset
 * calculation without memory access.
 *
 * However, virtual mappings need a page table and TLBs. Many Linux
 * architectures already map their physical space using 1-1 mappings
 * via TLBs. For those arches the virtual memory map is essentially
 * for free if we use the same page size as the 1-1 mappings. In that
 * case the overhead consists of a few additional pages that are
 * allocated to create a view of memory for vmemmap.
 *
 * The architecture is expected to provide a vmemmap_populate() function
 * to instantiate the mapping.
 */
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/bootmem.h>
#include <linux/memremap.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/sched.h>
#include <asm/dma.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>

/*
 * Allocate a block of memory to be used to back the virtual memory map
 * or to back the page tables that are used to create the mapping.
 * Uses the main allocators if they are available, else bootmem.
 */

static void * __init_refok __earlyonly_bootmem_alloc(int node,
				unsigned long size,
				unsigned long align,
				unsigned long goal)
{
	return memblock_virt_alloc_try_nid(size, align, goal,
					    BOOTMEM_ALLOC_ACCESSIBLE, node);
}

static void *vmemmap_buf;
static void *vmemmap_buf_end;

void * __meminit vmemmap_alloc_block(unsigned long size, int node)
{
	/* If the main allocator is up use that, fallback to bootmem. */
	if (slab_is_available()) {											// 슬랩 할당자가 사용 가능한 경우
		struct page *page;

		if (node_state(node, N_HIGH_MEMORY))							// HIGH Memory 인 경우, (Arm64 에서 사용하지 않음)
			page = alloc_pages_node(
				node, GFP_KERNEL | __GFP_ZERO | __GFP_REPEAT,
				get_order(size));
		else															// Normal Case
			page = alloc_pages(											// 1 page alloc
				GFP_KERNEL | __GFP_ZERO | __GFP_REPEAT,
				get_order(size));
		if (page)														// 할당을 제대로 받은 경우
			return page_address(page);									// phys -> virt 변환 후 가상 메모리 전달
		return NULL;
	} else
		return __earlyonly_bootmem_alloc(node, size, size,				// memblock 에서 1 page alloc
				__pa(MAX_DMA_ADDRESS));
}

/* need to make sure size is all the same during early stage */
static void * __meminit alloc_block_buf(unsigned long size, int node)
{
	void *ptr;

	if (!vmemmap_buf)													// vmemmap_buf 할당이 없었을 경우
		return vmemmap_alloc_block(size, node);							// 메모리 할당 후 리턴

	/* take the from buf */
	ptr = (void *)ALIGN((unsigned long)vmemmap_buf, size);
	if (ptr + size > vmemmap_buf_end)									// vmemmap_buf 사이즈 보다 큰 경우
		return vmemmap_alloc_block(size, node);							// 메모리 할당 후 리턴

	vmemmap_buf = ptr + size;											// vmemmap_buf 시작 주소 업데이트

	return ptr;															// 주소 값 리턴
}

static unsigned long __meminit vmem_altmap_next_pfn(struct vmem_altmap *altmap)
{
	return altmap->base_pfn + altmap->reserve + altmap->alloc
		+ altmap->align;
}

static unsigned long __meminit vmem_altmap_nr_free(struct vmem_altmap *altmap)
{
	unsigned long allocated = altmap->alloc + altmap->align;

	if (altmap->free > allocated)
		return altmap->free - allocated;
	return 0;
}

/**
 * vmem_altmap_alloc - allocate pages from the vmem_altmap reservation
 * @altmap - reserved page pool for the allocation
 * @nr_pfns - size (in pages) of the allocation
 *
 * Allocations are aligned to the size of the request
 */
static unsigned long __meminit vmem_altmap_alloc(struct vmem_altmap *altmap,
		unsigned long nr_pfns)
{
	unsigned long pfn = vmem_altmap_next_pfn(altmap);
	unsigned long nr_align;

	nr_align = 1UL << find_first_bit(&nr_pfns, BITS_PER_LONG);
	nr_align = ALIGN(pfn, nr_align) - pfn;

	if (nr_pfns + nr_align > vmem_altmap_nr_free(altmap))
		return ULONG_MAX;
	altmap->alloc += nr_pfns;
	altmap->align += nr_align;
	return pfn + nr_align;
}

static void * __meminit altmap_alloc_block_buf(unsigned long size,
		struct vmem_altmap *altmap)
{
	unsigned long pfn, nr_pfns;
	void *ptr;

	if (size & ~PAGE_MASK) {
		pr_warn_once("%s: allocations must be multiple of PAGE_SIZE (%ld)\n",
				__func__, size);
		return NULL;
	}

	nr_pfns = size >> PAGE_SHIFT;
	pfn = vmem_altmap_alloc(altmap, nr_pfns);
	if (pfn < ULONG_MAX)
		ptr = __va(__pfn_to_phys(pfn));
	else
		ptr = NULL;
	pr_debug("%s: pfn: %#lx alloc: %ld align: %ld nr: %#lx\n",
			__func__, pfn, altmap->alloc, altmap->align, nr_pfns);

	return ptr;
}

/* need to make sure size is all the same during early stage */
void * __meminit __vmemmap_alloc_block_buf(unsigned long size, int node,
		struct vmem_altmap *altmap)
{
	if (altmap)
		return altmap_alloc_block_buf(size, altmap);
	return alloc_block_buf(size, node);
}

void __meminit vmemmap_verify(pte_t *pte, int node,
				unsigned long start, unsigned long end)
{
	unsigned long pfn = pte_pfn(*pte);
	int actual_node = early_pfn_to_nid(pfn);

	if (node_distance(actual_node, node) > LOCAL_DISTANCE)
		pr_warn("[%lx-%lx] potential offnode page_structs\n",
			start, end - 1);
}

pte_t * __meminit vmemmap_pte_populate(pmd_t *pmd, unsigned long addr, int node)
{
	pte_t *pte = pte_offset_kernel(pmd, addr);									// pte 엔트리 주소를 구한다.
	if (pte_none(*pte)) {
		pte_t entry;
		void *p = alloc_block_buf(PAGE_SIZE, node);								// 미리 할당받아 놓은 buf 에서 메모리 주소를 하나 가져옴 (없을 경우 새로 할당)
		if (!p)
			return NULL;
		entry = pfn_pte(__pa(p) >> PAGE_SHIFT, PAGE_KERNEL);					// 책) pte 엔트리가 할당한 mem_map 페이지를 가리키도록 매핑한다. (mem_map 변수 사용하지 않음, 확인 필요)
		set_pte_at(&init_mm, addr, pte, entry);
	}
	return pte;
}

pmd_t * __meminit vmemmap_pmd_populate(pud_t *pud, unsigned long addr, int node)
{
	pmd_t *pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd)) {
		void *p = vmemmap_alloc_block(PAGE_SIZE, node);
		if (!p)
			return NULL;
		pmd_populate_kernel(&init_mm, pmd, p);
	}
	return pmd;
}

pud_t * __meminit vmemmap_pud_populate(pgd_t *pgd, unsigned long addr, int node)
{
	pud_t *pud = pud_offset(pgd, addr);
	if (pud_none(*pud)) {
		void *p = vmemmap_alloc_block(PAGE_SIZE, node);
		if (!p)
			return NULL;
		pud_populate(&init_mm, pud, p);
	}
	return pud;
}

pgd_t * __meminit vmemmap_pgd_populate(unsigned long addr, int node)
{
	pgd_t *pgd = pgd_offset_k(addr);
	if (pgd_none(*pgd)) {
		void *p = vmemmap_alloc_block(PAGE_SIZE, node);					// 1 page allocation 진행
		if (!p)
			return NULL;
		pgd_populate(&init_mm, pgd, p);									// 할당받은 page를 pgd 매핑
	}
	return pgd;
}

int __meminit vmemmap_populate_basepages(unsigned long start,
					 unsigned long end, int node)
{
	unsigned long addr = start;
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	for (; addr < end; addr += PAGE_SIZE) {
		pgd = vmemmap_pgd_populate(addr, node);							// 1 page 메모리 할당 및 테이블 등록
		if (!pgd)
			return -ENOMEM;
		pud = vmemmap_pud_populate(pgd, addr, node);					// pud에 동일 시퀀스 반복
		if (!pud)
			return -ENOMEM;
		pmd = vmemmap_pmd_populate(pud, addr, node);					// pmd에 동일 시퀀스 반복
		if (!pmd)
			return -ENOMEM;
		pte = vmemmap_pte_populate(pmd, addr, node);					// pte 할당 및 테이블 등록
		if (!pte)
			return -ENOMEM;
		vmemmap_verify(pte, node, addr, addr + PAGE_SIZE);
	}

	return 0;
}

struct page * __meminit sparse_mem_map_populate(unsigned long pnum, int nid)
{
	unsigned long start;
	unsigned long end;
	struct page *map;

	map = pfn_to_page(pnum * PAGES_PER_SECTION);								// pfn to page
	start = (unsigned long)map;													// page 포인터를 unsigned long으로 변환
	end = (unsigned long)(map + PAGES_PER_SECTION);								// page 마지막 지점의 포인터를 unsigned long 으로 변환

	if (vmemmap_populate(start, end, nid))
		return NULL;

	return map;
}

void __init sparse_mem_maps_populate_node(struct page **map_map,
					  unsigned long pnum_begin,
					  unsigned long pnum_end,
					  unsigned long map_count, int nodeid)
{
	unsigned long pnum;
	unsigned long size = sizeof(struct page) * PAGES_PER_SECTION;
	void *vmemmap_buf_start;

	size = ALIGN(size, PMD_SIZE);
	vmemmap_buf_start = __earlyonly_bootmem_alloc(nodeid, size * map_count,
			 PMD_SIZE, __pa(MAX_DMA_ADDRESS));

	if (vmemmap_buf_start) {
		vmemmap_buf = vmemmap_buf_start;
		vmemmap_buf_end = vmemmap_buf_start + size * map_count;
	}

	for (pnum = pnum_begin; pnum < pnum_end; pnum++) {
		struct mem_section *ms;

		if (!present_section_nr(pnum))
			continue;

		map_map[pnum] = sparse_mem_map_populate(pnum, nodeid);
		if (map_map[pnum])
			continue;
		ms = __nr_to_section(pnum);
		pr_err("%s: sparsemem memory map backing failed some memory will not be available\n",
		       __func__);
		ms->section_mem_map = 0;
	}

	if (vmemmap_buf_start) {
		/* need to free left buf */
		memblock_free_early(__pa(vmemmap_buf),
				    vmemmap_buf_end - vmemmap_buf);
		vmemmap_buf = NULL;
		vmemmap_buf_end = NULL;
	}
}
