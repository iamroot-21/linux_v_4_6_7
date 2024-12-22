/*
 * mm/percpu-vm.c - vmalloc area based chunk allocation
 *
 * Copyright (C) 2010		SUSE Linux Products GmbH
 * Copyright (C) 2010		Tejun Heo <tj@kernel.org>
 *
 * This file is released under the GPLv2.
 *
 * Chunks are mapped into vmalloc areas and populated page by page.
 * This is the default chunk allocator.
 */

static struct page *pcpu_chunk_page(struct pcpu_chunk *chunk,
				    unsigned int cpu, int page_idx)
{
	/* must not be used on pre-mapped chunk */
	WARN_ON(chunk->immutable);

	return vmalloc_to_page((void *)pcpu_chunk_addr(chunk, cpu, page_idx));
}

/**
 * pcpu_get_pages - get temp pages array
 * @chunk: chunk of interest
 *
 * Returns pointer to array of pointers to struct page which can be indexed
 * with pcpu_page_idx().  Note that there is only one array and accesses
 * should be serialized by pcpu_alloc_mutex.
 *
 * RETURNS:
 * Pointer to temp pages array on success.
 */
static struct page **pcpu_get_pages(struct pcpu_chunk *chunk_alloc)
{
	/**
	 * @brief 임시 페이지 배열을 할당한다.
	 */
	static struct page **pages;
	size_t pages_size = pcpu_nr_units * pcpu_unit_pages * sizeof(pages[0]);

	lockdep_assert_held(&pcpu_alloc_mutex);

	if (!pages)
		pages = pcpu_mem_zalloc(pages_size); // pages 배열 할당
	return pages; // 할당받은 pages 배열 리턴
}

/**
 * pcpu_free_pages - free pages which were allocated for @chunk
 * @chunk: chunk pages were allocated for
 * @pages: array of pages to be freed, indexed by pcpu_page_idx()
 * @page_start: page index of the first page to be freed
 * @page_end: page index of the last page to be freed + 1
 *
 * Free pages [@page_start and @page_end) in @pages for all units.
 * The pages were allocated for @chunk.
 */
static void pcpu_free_pages(struct pcpu_chunk *chunk,
			    struct page **pages, int page_start, int page_end)
{
	unsigned int cpu;
	int i;

	for_each_possible_cpu(cpu) { // cpu 별로 loop
		for (i = page_start; i < page_end; i++) {
			struct page *page = pages[pcpu_page_idx(cpu, i)]; // pages에서 cpu 별로 할당 영역의 페이지를 참조조

			if (page) // 해당 page가 할당받은 상태인 경우
				__free_page(page); // 할당 해제제
		}
	}
}

/**
 * pcpu_alloc_pages - allocates pages for @chunk
 * @chunk: target chunk
 * @pages: array to put the allocated pages into, indexed by pcpu_page_idx()
 * @page_start: page index of the first page to be allocated
 * @page_end: page index of the last page to be allocated + 1
 *
 * Allocate pages [@page_start,@page_end) into @pages for all units.
 * The allocation is for @chunk.  Percpu core doesn't care about the
 * content of @pages and will pass it verbatim to pcpu_map_pages().
 */
static int pcpu_alloc_pages(struct pcpu_chunk *chunk,
			    struct page **pages, int page_start, int page_end)
{
	/**
	 * @brief 
	 */
	const gfp_t gfp = GFP_KERNEL | __GFP_HIGHMEM | __GFP_COLD;
	unsigned int cpu, tcpu;
	int i;

	for_each_possible_cpu(cpu) { // pages를 초기화하기 위해 cpu 별로 indexing
		for (i = page_start; i < page_end; i++) {
			struct page **pagep = &pages[pcpu_page_idx(cpu, i)]; // cpu마다 할당된 pages를 참조

			*pagep = alloc_pages_node(cpu_to_node(cpu), gfp, 0); // page 할당 이후 pagep에 값 입력
			if (!*pagep) // 할당이 실패한 경우 에러 케이스
				goto err;
		}
	}
	return 0;

err: // 에러 케이스 처리
	while (--i >= page_start) // 할당 받았던 만큼 Loop
		__free_page(pages[pcpu_page_idx(cpu, i)]); // 할당 받았던 page 할당 해제

	for_each_possible_cpu(tcpu) { // cpu 별로 loop
		if (tcpu == cpu)
			break;
		for (i = page_start; i < page_end; i++)
			__free_page(pages[pcpu_page_idx(tcpu, i)]); // cpu 별로 할당받았던 page 할당 해제
	}
	return -ENOMEM; // Error 플래그 리턴
}

/**
 * pcpu_pre_unmap_flush - flush cache prior to unmapping
 * @chunk: chunk the regions to be flushed belongs to
 * @page_start: page index of the first page to be flushed
 * @page_end: page index of the last page to be flushed + 1
 *
 * Pages in [@page_start,@page_end) of @chunk are about to be
 * unmapped.  Flush cache.  As each flushing trial can be very
 * expensive, issue flush on the whole region at once rather than
 * doing it for each cpu.  This could be an overkill but is more
 * scalable.
 */
static void pcpu_pre_unmap_flush(struct pcpu_chunk *chunk,
				 int page_start, int page_end)
{
	flush_cache_vunmap(
		pcpu_chunk_addr(chunk, pcpu_low_unit_cpu, page_start),
		pcpu_chunk_addr(chunk, pcpu_high_unit_cpu, page_end));
}

static void __pcpu_unmap_pages(unsigned long addr, int nr_pages)
{
	unmap_kernel_range_noflush(addr, nr_pages << PAGE_SHIFT);
}

/**
 * pcpu_unmap_pages - unmap pages out of a pcpu_chunk
 * @chunk: chunk of interest
 * @pages: pages array which can be used to pass information to free
 * @page_start: page index of the first page to unmap
 * @page_end: page index of the last page to unmap + 1
 *
 * For each cpu, unmap pages [@page_start,@page_end) out of @chunk.
 * Corresponding elements in @pages were cleared by the caller and can
 * be used to carry information to pcpu_free_pages() which will be
 * called after all unmaps are finished.  The caller should call
 * proper pre/post flush functions.
 */
static void pcpu_unmap_pages(struct pcpu_chunk *chunk,
			     struct page **pages, int page_start, int page_end)
{
	unsigned int cpu;
	int i;

	for_each_possible_cpu(cpu) {
		for (i = page_start; i < page_end; i++) {
			struct page *page;

			page = pcpu_chunk_page(chunk, cpu, i);
			WARN_ON(!page);
			pages[pcpu_page_idx(cpu, i)] = page;
		}
		__pcpu_unmap_pages(pcpu_chunk_addr(chunk, cpu, page_start),
				   page_end - page_start);
	}
}

/**
 * pcpu_post_unmap_tlb_flush - flush TLB after unmapping
 * @chunk: pcpu_chunk the regions to be flushed belong to
 * @page_start: page index of the first page to be flushed
 * @page_end: page index of the last page to be flushed + 1
 *
 * Pages [@page_start,@page_end) of @chunk have been unmapped.  Flush
 * TLB for the regions.  This can be skipped if the area is to be
 * returned to vmalloc as vmalloc will handle TLB flushing lazily.
 *
 * As with pcpu_pre_unmap_flush(), TLB flushing also is done at once
 * for the whole region.
 */
static void pcpu_post_unmap_tlb_flush(struct pcpu_chunk *chunk,
				      int page_start, int page_end)
{
	flush_tlb_kernel_range(
		pcpu_chunk_addr(chunk, pcpu_low_unit_cpu, page_start),
		pcpu_chunk_addr(chunk, pcpu_high_unit_cpu, page_end));
}

static int __pcpu_map_pages(unsigned long addr, struct page **pages,
			    int nr_pages)
{
	return map_kernel_range_noflush(addr, nr_pages << PAGE_SHIFT,
					PAGE_KERNEL, pages);
}

/**
 * pcpu_map_pages - map pages into a pcpu_chunk
 * @chunk: chunk of interest
 * @pages: pages array containing pages to be mapped
 * @page_start: page index of the first page to map
 * @page_end: page index of the last page to map + 1
 *
 * For each cpu, map pages [@page_start,@page_end) into @chunk.  The
 * caller is responsible for calling pcpu_post_map_flush() after all
 * mappings are complete.
 *
 * This function is responsible for setting up whatever is necessary for
 * reverse lookup (addr -> chunk).
 */
static int pcpu_map_pages(struct pcpu_chunk *chunk,
			  struct page **pages, int page_start, int page_end)
{
	/**
	 * @brief 할당받은 페이지를 vmalloc 영역에 매핑한다.
	 */
	unsigned int cpu, tcpu;
	int i, err;

	for_each_possible_cpu(cpu) { // 사용 가능한 cpu 별로 loop
		err = __pcpu_map_pages(pcpu_chunk_addr(chunk, cpu, page_start), // vmalloc 영역에 매핑
				       &pages[pcpu_page_idx(cpu, page_start)],
				       page_end - page_start);
		if (err < 0) // error case 처리
			goto err;

		for (i = page_start; i < page_end; i++) // 해당 cpu가 사용 중인 page 별로 loop
			pcpu_set_page_chunk(pages[pcpu_page_idx(cpu, i)], // 각 페이지들이 pcpu_chunk를 가리키도록 설정
					    chunk);
	}
	return 0;
err: // error case
	for_each_possible_cpu(tcpu) { // cpu 별로 loop
		if (tcpu == cpu)
			break;
		__pcpu_unmap_pages(pcpu_chunk_addr(chunk, tcpu, page_start), // error case 이전까지 mapping 했던 chunk unmapping
				   page_end - page_start);
	}
	pcpu_post_unmap_tlb_flush(chunk, page_start, page_end); // TLB Flush
	return err;
}

/**
 * pcpu_post_map_flush - flush cache after mapping
 * @chunk: pcpu_chunk the regions to be flushed belong to
 * @page_start: page index of the first page to be flushed
 * @page_end: page index of the last page to be flushed + 1
 *
 * Pages [@page_start,@page_end) of @chunk have been mapped.  Flush
 * cache.
 *
 * As with pcpu_pre_unmap_flush(), TLB flushing also is done at once
 * for the whole region.
 */
static void pcpu_post_map_flush(struct pcpu_chunk *chunk,
				int page_start, int page_end)
{
	flush_cache_vmap(
		pcpu_chunk_addr(chunk, pcpu_low_unit_cpu, page_start),
		pcpu_chunk_addr(chunk, pcpu_high_unit_cpu, page_end));
}

/**
 * pcpu_populate_chunk - populate and map an area of a pcpu_chunk
 * @chunk: chunk of interest
 * @page_start: the start page
 * @page_end: the end page
 *
 * For each cpu, populate and map pages [@page_start,@page_end) into
 * @chunk.
 *
 * CONTEXT:
 * pcpu_alloc_mutex, does GFP_KERNEL allocation.
 */
static int pcpu_populate_chunk(struct pcpu_chunk *chunk,
			       int page_start, int page_end)
{
	/**
	 * @brief chunk 내의 요청된 페이지 범위를 활성화한다.
	 */
	struct page **pages;

	pages = pcpu_get_pages(chunk); // pages 배열을 할당받아옴
	if (!pages) // 할당 실패 케이스
		return -ENOMEM;

	if (pcpu_alloc_pages(chunk, pages, page_start, page_end)) // 필요 페이지 범위를 cpu 개수만큼 할당받는다.
		return -ENOMEM;

	if (pcpu_map_pages(chunk, pages, page_start, page_end)) { // TODO 4-184) 할당받은 페이지들을 vmalloc 영역에 매핑
		pcpu_free_pages(chunk, pages, page_start, page_end); // 할당받았던 pages를 할당 해제
		return -ENOMEM;
	}
	pcpu_post_map_flush(chunk, page_start, page_end); // TLB 캐시 플러시

	return 0;
}

/**
 * pcpu_depopulate_chunk - depopulate and unmap an area of a pcpu_chunk
 * @chunk: chunk to depopulate
 * @page_start: the start page
 * @page_end: the end page
 *
 * For each cpu, depopulate and unmap pages [@page_start,@page_end)
 * from @chunk.
 *
 * CONTEXT:
 * pcpu_alloc_mutex.
 */
static void pcpu_depopulate_chunk(struct pcpu_chunk *chunk,
				  int page_start, int page_end)
{
	struct page **pages;

	/*
	 * If control reaches here, there must have been at least one
	 * successful population attempt so the temp pages array must
	 * be available now.
	 */
	pages = pcpu_get_pages(chunk);
	BUG_ON(!pages);

	/* unmap and free */
	pcpu_pre_unmap_flush(chunk, page_start, page_end);

	pcpu_unmap_pages(chunk, pages, page_start, page_end);

	/* no need to flush tlb, vmalloc will handle it lazily */

	pcpu_free_pages(chunk, pages, page_start, page_end);
}

static struct pcpu_chunk *pcpu_create_chunk(void)
{
	/**
	 * @brief chunk를 하나 생성한다.
	 */
	struct pcpu_chunk *chunk;
	struct vm_struct **vms;

	chunk = pcpu_alloc_chunk(); // 1개 chunk 및 chunk->map 을 할당, 초기화
	if (!chunk)
		return NULL;

	vms = pcpu_get_vm_areas(pcpu_group_offsets, pcpu_group_sizes,
				pcpu_nr_groups, pcpu_atom_size);
	if (!vms) {
		pcpu_free_chunk(chunk);
		return NULL;
	}

	chunk->data = vms;
	chunk->base_addr = vms[0]->addr - pcpu_group_offsets[0];
	return chunk;
}

static void pcpu_destroy_chunk(struct pcpu_chunk *chunk)
{
	if (chunk && chunk->data)
		pcpu_free_vm_areas(chunk->data, pcpu_nr_groups);
	pcpu_free_chunk(chunk);
}

static struct page *pcpu_addr_to_page(void *addr)
{
	return vmalloc_to_page(addr);
}

static int __init pcpu_verify_alloc_info(const struct pcpu_alloc_info *ai)
{
	/* no extra restriction */
	return 0;
}
