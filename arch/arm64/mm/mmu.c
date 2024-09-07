/*
 * Based on arch/arm/mm/mmu.c
 *
 * Copyright (C) 1995-2005 Russell King
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/libfdt.h>
#include <linux/mman.h>
#include <linux/nodemask.h>
#include <linux/memblock.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/stop_machine.h>

#include <asm/barrier.h>
#include <asm/cputype.h>
#include <asm/fixmap.h>
#include <asm/kasan.h>
#include <asm/kernel-pgtable.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/sizes.h>
#include <asm/tlb.h>
#include <asm/memblock.h>
#include <asm/mmu_context.h>

#include "mm.h"

u64 idmap_t0sz = TCR_T0SZ(VA_BITS);

u64 kimage_voffset __read_mostly;
EXPORT_SYMBOL(kimage_voffset);

/*
 * Empty_zero_page is a special page that is used for zero-initialized data
 * and COW.
 */
unsigned long empty_zero_page[PAGE_SIZE / sizeof(unsigned long)] __page_aligned_bss;
EXPORT_SYMBOL(empty_zero_page);

static pte_t bm_pte[PTRS_PER_PTE] __page_aligned_bss;
static pmd_t bm_pmd[PTRS_PER_PMD] __page_aligned_bss __maybe_unused;
static pud_t bm_pud[PTRS_PER_PUD] __page_aligned_bss __maybe_unused;

pgprot_t phys_mem_access_prot(struct file *file, unsigned long pfn,
			      unsigned long size, pgprot_t vma_prot)
{
	if (!pfn_valid(pfn))
		return pgprot_noncached(vma_prot);
	else if (file->f_flags & O_SYNC)
		return pgprot_writecombine(vma_prot);
	return vma_prot;
}
EXPORT_SYMBOL(phys_mem_access_prot);

static phys_addr_t __init early_pgtable_alloc(void)
{
	phys_addr_t phys;
	void *ptr;

	phys = memblock_alloc(PAGE_SIZE, PAGE_SIZE); // 메모리 블록의 물리 주소를 할당 받는다.
	BUG_ON(!phys);

	/*
	 * The FIX_{PGD,PUD,PMD} slots may be in active use, but the FIX_PTE
	 * slot will be free, so we can (ab)use the FIX_PTE slot to initialise
	 * any level of table.
	 */
	// *** phys 를 0으로 초기화해야 하지만, 물리 주소이므로 가상 주소로 변환해야 할 필요가 있음
	ptr = pte_set_fixmap(phys); // memblock_alloc으로 할당받은 물리 주소를 fixmap 가상 주소에 매핑, 가상 주소를 리턴 받음

	memset(ptr, 0, PAGE_SIZE); // phys 주소를 0으로 초기화 함

	/*
	 * Implicit barriers also ensure the zeroed page is visible to the page
	 * table walker
	 */
	pte_clear_fixmap(); // pte가 가리키고 있는 주소를 초기화

	return phys; // memblock_alloc 에서 할당받았던 물리 메모리 주소를 리턴
}

/*
 * remap a PMD into pages
 */
static void split_pmd(pmd_t *pmd, pte_t *pte)
{
	unsigned long pfn = pmd_pfn(*pmd);
	int i = 0;

	do {
		/*
		 * Need to have the least restrictive permissions available
		 * permissions will be fixed up later
		 */
		set_pte(pte, pfn_pte(pfn, PAGE_KERNEL_EXEC));
		pfn++;
	} while (pte++, i++, i < PTRS_PER_PTE);
}

static void alloc_init_pte(pmd_t *pmd, unsigned long addr,
				  unsigned long end, unsigned long pfn,
				  pgprot_t prot,
				  phys_addr_t (*pgtable_alloc)(void))
{
	pte_t *pte;

	if (pmd_none(*pmd) || pmd_sect(*pmd)) { // pmd entry가 null인 경우 or 2MB section 할당받는 경우
		phys_addr_t pte_phys;
		BUG_ON(!pgtable_alloc);
		pte_phys = pgtable_alloc(); // 함수 포인터로 받아온 early_pgtable_alloc 함수 실행, pte를 생성
		pte = pte_set_fixmap(pte_phys); // pte 가상 주소에 매핑
		if (pmd_sect(*pmd))
			split_pmd(pmd, pte);  // pmd 섹션 페이지를 pte 테이블 형태로 분리 512개
		__pmd_populate(pmd, pte_phys, PMD_TYPE_TABLE);  // pmd = pte
		flush_tlb_all();
		pte_clear_fixmap(); // pte fixmap 초기화
	}
	BUG_ON(pmd_bad(*pmd));

	pte = pte_set_fixmap_offset(pmd, addr); // pmd가상 주소에 매핑
	do {
		set_pte(pte, pfn_pte(pfn, prot));  // pte = pfn = prot, 물리 주소를 pte에 할당 
		pfn++;
	} while (pte++, addr += PAGE_SIZE, addr != end);

	pte_clear_fixmap(); // pte fixmap 초기화
}

static void split_pud(pud_t *old_pud, pmd_t *pmd)
{
	unsigned long addr = pud_pfn(*old_pud) << PAGE_SHIFT;
	pgprot_t prot = __pgprot(pud_val(*old_pud) ^ addr);
	int i = 0;

	do {
		set_pmd(pmd, __pmd(addr | pgprot_val(prot)));
		addr += PMD_SIZE;
	} while (pmd++, i++, i < PTRS_PER_PMD);
}

#ifdef CONFIG_DEBUG_PAGEALLOC
static bool block_mappings_allowed(phys_addr_t (*pgtable_alloc)(void))
{

	/*
	 * If debug_page_alloc is enabled we must map the linear map
	 * using pages. However, other mappings created by
	 * create_mapping_noalloc must use sections in some cases. Allow
	 * sections to be used in those cases, where no pgtable_alloc
	 * function is provided.
	 */
	return !pgtable_alloc || !debug_pagealloc_enabled();
}
#else
static bool block_mappings_allowed(phys_addr_t (*pgtable_alloc)(void))
{
	return true;
}
#endif

static void alloc_init_pmd(pud_t *pud, unsigned long addr, unsigned long end,
				  phys_addr_t phys, pgprot_t prot,
				  phys_addr_t (*pgtable_alloc)(void))
{
	pmd_t *pmd;
	unsigned long next;

	/*
	 * Check for initial section mappings in the pgd/pud and remove them.
	 */
	if (pud_none(*pud) || pud_sect(*pud)) { // pud entry가 null인 경우 or 1GB section 할당받는 경우
		phys_addr_t pmd_phys;
		BUG_ON(!pgtable_alloc);
		pmd_phys = pgtable_alloc(); // 함수 포인터로 받아온 early_pgtable_alloc 함수 실행, pmd를 생성
		pmd = pmd_set_fixmap(pmd_phys); // pmd 가상 주소에 매핑
		if (pud_sect(*pud)) {
			/*
			 * need to have the 1G of mappings continue to be
			 * present
			 */
			split_pud(pud, pmd); // pud 섹션 페이지를 pmd 테이블 형태로 분리 512개
		}
		__pud_populate(pud, pmd_phys, PUD_TYPE_TABLE); // pud = pmd
		flush_tlb_all();
		pmd_clear_fixmap(); // pmd fixmap 초기화
	}
	BUG_ON(pud_bad(*pud));

	pmd = pmd_set_fixmap_offset(pud, addr);  // pud 가상 주소에 매핑
	do {
		next = pmd_addr_end(addr, end);  // 다음 pmd 엔드리 가상 주소를 리턴 (next = end)
		/* try section mapping first */
		if (((addr | next | phys) & ~SECTION_MASK) == 0 && // Use 2MB Block 인 경우
		      block_mappings_allowed(pgtable_alloc)) {
			pmd_t old_pmd =*pmd;
			pmd_set_huge(pmd, phys, prot);  // TLB 캐시 미스 횟수를 줄이기 위해 pmd 사이즈를 2MB 엔트리 1개로 생성 (최적화 옵션) http://www.iamroot.org/xe/index.php?mid=Programming&document_srl=208966
			/*
			 * Check for previous table entries created during
			 * boot (__create_page_tables) and flush them.
			 */
			if (!pmd_none(old_pmd)) { // pmd에 이미 할당된 테이블이 있을 경우 할당 해제
				flush_tlb_all();
				if (pmd_table(old_pmd)) {
					phys_addr_t table = pmd_page_paddr(old_pmd);
					if (!WARN_ON_ONCE(slab_is_available()))
						memblock_free(table, PAGE_SIZE);
				}
			}
		} else {
			alloc_init_pte(pmd, addr, next, __phys_to_pfn(phys), // TODO 3-10
				       prot, pgtable_alloc);
		}
		phys += next - addr;
	} while (pmd++, addr = next, addr != end);

	pmd_clear_fixmap(); // pmd용 fixmap pager unmapping
}

static inline bool use_1G_block(unsigned long addr, unsigned long next,
			unsigned long phys)
{
	if (PAGE_SHIFT != 12)
		return false;

	if (((addr | next | phys) & ~PUD_MASK) != 0)
		return false;

	return true;
}

static void alloc_init_pud(pgd_t *pgd, unsigned long addr, unsigned long end,
				  phys_addr_t phys, pgprot_t prot,
				  phys_addr_t (*pgtable_alloc)(void))
{
	pud_t *pud;
	unsigned long next;

	if (pgd_none(*pgd)) { // pud가 존재하지 않는 경우 pud 생성
		phys_addr_t pud_phys;
		BUG_ON(!pgtable_alloc);
		pud_phys = pgtable_alloc(); // 함수 포인터로 받아온 early_pgtable_alloc 함수 실행, pud를 생성
		__pgd_populate(pgd, pud_phys, PUD_TYPE_TABLE); // pgd에 새로 할당한 pud 입력 (*pgd = pud_phys)
	}
	BUG_ON(pgd_bad(*pgd));

	pud = pud_set_fixmap_offset(pgd, addr); // pgd 가상 주소에 매핑
	do {
		next = pud_addr_end(addr, end); // 다음 pud 엔드리 가상 주소를 리턴 (next = end)

		/*
		 * For 4K granule only, attempt to put down a 1GB block
		 */
		if (use_1G_block(addr, next, phys) && // 4KB, pud를 사용해야 하는 경우
		    block_mappings_allowed(pgtable_alloc)) {
			pud_t old_pud = *pud;
			pud_set_huge(pud, phys, prot); // TLB 캐시 미스 횟수를 줄이기 위해 pud를 1GB 엔트리 1개로 생성 (최적화 옵션) http://www.iamroot.org/xe/index.php?mid=Programming&document_srl=208966

			/*
			 * If we have an old value for a pud, it will
			 * be pointing to a pmd table that we no longer
			 * need (from swapper_pg_dir).
			 *
			 * Look up the old pmd table and free it.
			 */
			if (!pud_none(old_pud)) { // pud 테이블에 입력된 값이 있을 경우 (pmd 가 있는 경우) 이를 할당 해제
				flush_tlb_all();
				if (pud_table(old_pud)) {
					phys_addr_t table = pud_page_paddr(old_pud);
					if (!WARN_ON_ONCE(slab_is_available()))
						memblock_free(table, PAGE_SIZE);
				}
			}
		} else {
			alloc_init_pmd(pud, addr, next, phys, prot, // ** TODO 3-9
				       pgtable_alloc);
		}
		phys += next - addr;
	} while (pud++, addr = next, addr != end);

	pud_clear_fixmap(); // pud용 fixmap pager unmapping
}

/*
 * Create the page directory entries and any necessary page tables for the
 * mapping specified by 'md'.
 */
static void init_pgd(pgd_t *pgd, phys_addr_t phys, unsigned long virt,
				    phys_addr_t size, pgprot_t prot,
				    phys_addr_t (*pgtable_alloc)(void))
{
	unsigned long addr, length, end, next;

	/*
	 * If the virtual and physical address don't have the same offset
	 * within a page, we cannot map the region as the caller expects.
	 */
	if (WARN_ON((phys ^ virt) & ~PAGE_MASK)) // 물리 메모리와 가상 메모리 오프셋이 동일하지 않은 경우 에러 처리
		return;

	phys &= PAGE_MASK; // 물리 메모리에서 page offset 부분을 마스킹
	addr = virt & PAGE_MASK; // 가상 메모리에서 pag offset 부분을 마스킹
	length = PAGE_ALIGN(size + (virt & ~PAGE_MASK)); // 다음 페이지 시작주소까지 올림하여 할당 크기계산

	end = addr + length; // 마지막 지점 계산
	do {
		next = pgd_addr_end(addr, end); // 다음 pgd 주소를 가져온다.
		alloc_init_pud(pgd, addr, next, phys, prot, pgtable_alloc); // 하위 PUD 테이블 할당부로 추정 (***TODO 분석 필요)
		phys += next - addr; // phys 에 size 만큼 더한다.
	} while (pgd++, addr = next, addr != end); // pgd 전체 순회
}

static phys_addr_t late_pgtable_alloc(void)
{
	void *ptr = (void *)__get_free_page(PGALLOC_GFP);
	BUG_ON(!ptr);

	/* Ensure the zeroed page is visible to the page table walker */
	dsb(ishst);
	return __pa(ptr);
}

static void __create_pgd_mapping(pgd_t *pgdir, phys_addr_t phys,
				 unsigned long virt, phys_addr_t size,
				 pgprot_t prot,
				 phys_addr_t (*alloc)(void))
{
	init_pgd(pgd_offset_raw(pgdir, virt), phys, virt, size, prot, alloc);
}

/*
 * This function can only be used to modify existing table entries,
 * without allocating new levels of table. Note that this permits the
 * creation of new section or page entries.
 */
static void __init create_mapping_noalloc(phys_addr_t phys, unsigned long virt,
				  phys_addr_t size, pgprot_t prot)
{
	if (virt < VMALLOC_START) {
		pr_warn("BUG: not creating mapping for %pa at 0x%016lx - outside kernel range\n",
			&phys, virt);
		return;
	}
	__create_pgd_mapping(init_mm.pgd, phys, virt, size, prot,
			     NULL);
}

void __init create_pgd_mapping(struct mm_struct *mm, phys_addr_t phys,
			       unsigned long virt, phys_addr_t size,
			       pgprot_t prot)
{
	__create_pgd_mapping(mm->pgd, phys, virt, size, prot,
			     late_pgtable_alloc);
}

static void create_mapping_late(phys_addr_t phys, unsigned long virt,
				  phys_addr_t size, pgprot_t prot)
{
	if (virt < VMALLOC_START) {
		pr_warn("BUG: not creating mapping for %pa at 0x%016lx - outside kernel range\n",
			&phys, virt);
		return;
	}

	__create_pgd_mapping(init_mm.pgd, phys, virt, size, prot,
			     late_pgtable_alloc);
}

static void __init __map_memblock(pgd_t *pgd, phys_addr_t start, phys_addr_t end)
{
	unsigned long kernel_start = __pa(_stext);
	unsigned long kernel_end = __pa(_etext);

	/*
	 * Take care not to create a writable alias for the
	 * read-only text and rodata sections of the kernel image.
	 */

	/* No overlap with the kernel text */
	if (end < kernel_start || start >= kernel_end) {
		__create_pgd_mapping(pgd, start, __phys_to_virt(start),
				     end - start, PAGE_KERNEL,
				     early_pgtable_alloc);
		return;
	}

	/*
	 * This block overlaps the kernel text mapping.
	 * Map the portion(s) which don't overlap.
	 */
	if (start < kernel_start)
		__create_pgd_mapping(pgd, start,
				     __phys_to_virt(start),
				     kernel_start - start, PAGE_KERNEL,
				     early_pgtable_alloc);
	if (kernel_end < end)
		__create_pgd_mapping(pgd, kernel_end,
				     __phys_to_virt(kernel_end),
				     end - kernel_end, PAGE_KERNEL,
				     early_pgtable_alloc);

	/*
	 * Map the linear alias of the [_stext, _etext) interval as
	 * read-only/non-executable. This makes the contents of the
	 * region accessible to subsystems such as hibernate, but
	 * protects it from inadvertent modification or execution.
	 */
	__create_pgd_mapping(pgd, kernel_start, __phys_to_virt(kernel_start),
			     kernel_end - kernel_start, PAGE_KERNEL_RO,
			     early_pgtable_alloc);
}

static void __init map_mem(pgd_t *pgd)
{
	struct memblock_region *reg;

	/* map all the memory banks */
	for_each_memblock(memory, reg) { // memory region 을 순서대로 가져온다.
		phys_addr_t start = reg->base; // region 시작 주소
		phys_addr_t end = start + reg->size; // region 사이즈

		if (start >= end)
			break;
		if (memblock_is_nomap(reg)) // no mapping 인 경우 mapping 하지 않음
			continue;

		__map_memblock(pgd, start, end); // fdt 에서 읽어온 memory region 을 pgd 에 매핑, 커널 이미지와 겹치는 부분은 읽기 전용으로 매핑
	}
}

void mark_rodata_ro(void)
{
	unsigned long section_size;

	section_size = (unsigned long)__start_rodata - (unsigned long)_stext;
	create_mapping_late(__pa(_stext), (unsigned long)_stext,
			    section_size, PAGE_KERNEL_ROX);
	/*
	 * mark .rodata as read only. Use _etext rather than __end_rodata to
	 * cover NOTES and EXCEPTION_TABLE.
	 */
	section_size = (unsigned long)_etext - (unsigned long)__start_rodata;
	create_mapping_late(__pa(__start_rodata), (unsigned long)__start_rodata,
			    section_size, PAGE_KERNEL_RO);
}

void fixup_init(void)
{
	/*
	 * Unmap the __init region but leave the VM area in place. This
	 * prevents the region from being reused for kernel modules, which
	 * is not supported by kallsyms.
	 */
	unmap_kernel_range((u64)__init_begin, (u64)(__init_end - __init_begin));
}

static void __init map_kernel_chunk(pgd_t *pgd, void *va_start, void *va_end,
				    pgprot_t prot, struct vm_struct *vma)
{
	phys_addr_t pa_start = __pa(va_start); // 영역 시작 주소
	unsigned long size = va_end - va_start; // 영역 사이즈

	BUG_ON(!PAGE_ALIGNED(pa_start)); // 페이지 사이즈 정렬 여부 확인
	BUG_ON(!PAGE_ALIGNED(size));

	__create_pgd_mapping(pgd, pa_start, (unsigned long)va_start, size, prot,
			     early_pgtable_alloc); // 요청 가상 주소 범위를 가상 주소에 해당하는 물리 주소에 prot 메모리 타입으로 매핑 (***TODO 3-7 Code 분석)

	// vm_struct 값 입력
	vma->addr	= va_start;
	vma->phys_addr	= pa_start;
	vma->size	= size;
	vma->flags	= VM_MAP;
	vma->caller	= __builtin_return_address(0);

	vm_area_add_early(vma); // vm list에 추가한다.
}

/*
 * Create fine-grained mappings for the kernel.
 */
static void __init map_kernel(pgd_t *pgd)
{
	static struct vm_struct vmlinux_text, vmlinux_rodata, vmlinux_init, vmlinux_data;

	map_kernel_chunk(pgd, _stext, __start_rodata, PAGE_KERNEL_EXEC, &vmlinux_text); // 커널 이미지의 일반 코드 영역을 커널 실행 페이지 타입으로 매핑 
	map_kernel_chunk(pgd, __start_rodata, _etext, PAGE_KERNEL, &vmlinux_rodata); // 읽기 전용 데이터 영역을 커널 페이지 타입으로 매핑
	map_kernel_chunk(pgd, __init_begin, __init_end, PAGE_KERNEL_EXEC, // 초기화 코드 및 초기화 데이터 영역을 커널 실행 페이지 타입으로 매핑
			 &vmlinux_init);
	map_kernel_chunk(pgd, _data, _end, PAGE_KERNEL, &vmlinux_data); // 일반 데이터 영역을 커널 페이지 타입으로 매핑

	if (!pgd_val(*pgd_offset_raw(pgd, FIXADDR_START))) { // pgd에 값이 들어있지 않는 경우
		/*
		 * The fixmap falls in a separate pgd to the kernel, and doesn't
		 * live in the carveout for the swapper_pg_dir. We can simply
		 * re-use the existing dir for the fixmap.
		 */
		set_pgd(pgd_offset_raw(pgd, FIXADDR_START), // pgd 값을 입력
			*pgd_offset_k(FIXADDR_START));
	} else if (CONFIG_PGTABLE_LEVELS > 3) { // LEVEL 4 이상인 경우
		/*
		 * The fixmap shares its top level pgd entry with the kernel
		 * mapping. This can really only occur when we are running
		 * with 16k/4 levels, so we can simply reuse the pud level
		 * entry instead.
		 */
		BUG_ON(!IS_ENABLED(CONFIG_ARM64_16K_PAGES)); // 16K Paging이 아닌 경우 에러
		set_pud(pud_set_fixmap_offset(pgd, FIXADDR_START), // pud 값을 입력
			__pud(__pa(bm_pmd) | PUD_TYPE_TABLE));
		pud_clear_fixmap(); // PUD의 fixmap 초기화
	} else {
		BUG();
	}

	kasan_copy_shadow(pgd); // init_pg_dir에 매핑되어 있는 kasan shadow region 영역을 swapper_pg_dir로 복사 매핑한다 **사용하지 않음
}

/*
 * paging_init() sets up the page tables, initialises the zone memory
 * maps and sets up the zero page.
 */
void __init paging_init(void)
{
	phys_addr_t pgd_phys = early_pgtable_alloc();
	pgd_t *pgd = pgd_set_fixmap(pgd_phys);

	map_kernel(pgd);
	map_mem(pgd);

	/*
	 * We want to reuse the original swapper_pg_dir so we don't have to
	 * communicate the new address to non-coherent secondaries in
	 * secondary_entry, and so cpu_switch_mm can generate the address with
	 * adrp+add rather than a load from some global variable.
	 *
	 * To do this we need to go via a temporary pgd.
	 */
	cpu_replace_ttbr1(__va(pgd_phys));
	memcpy(swapper_pg_dir, pgd, PAGE_SIZE);
	cpu_replace_ttbr1(swapper_pg_dir);

	pgd_clear_fixmap();
	memblock_free(pgd_phys, PAGE_SIZE);

	/*
	 * We only reuse the PGD from the swapper_pg_dir, not the pud + pmd
	 * allocated with it.
	 */
	memblock_free(__pa(swapper_pg_dir) + PAGE_SIZE,
		      SWAPPER_DIR_SIZE - PAGE_SIZE);

	bootmem_init();
}

/*
 * Check whether a kernel address is valid (derived from arch/x86/).
 */
int kern_addr_valid(unsigned long addr)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	if ((((long)addr) >> VA_BITS) != -1UL)
		return 0;

	pgd = pgd_offset_k(addr);
	if (pgd_none(*pgd))
		return 0;

	pud = pud_offset(pgd, addr);
	if (pud_none(*pud))
		return 0;

	if (pud_sect(*pud))
		return pfn_valid(pud_pfn(*pud));

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd))
		return 0;

	if (pmd_sect(*pmd))
		return pfn_valid(pmd_pfn(*pmd));

	pte = pte_offset_kernel(pmd, addr);
	if (pte_none(*pte))
		return 0;

	return pfn_valid(pte_pfn(*pte));
}
#define CONFIG_SPARSEMEM_VMEMMAP														// 임시 선언
#ifdef CONFIG_SPARSEMEM_VMEMMAP
#define ARM64_SWAPPER_USES_SECTION_MAPS 1												// 임시 선언
#if !ARM64_SWAPPER_USES_SECTION_MAPS
int __meminit vmemmap_populate(unsigned long start, unsigned long end, int node)
{
	return vmemmap_populate_basepages(start, end, node);
}
#else	/* !ARM64_SWAPPER_USES_SECTION_MAPS */
int __meminit vmemmap_populate(unsigned long start, unsigned long end, int node)
{
	/**
	 * \arg [in]start page 포인터의 주소 값
	 * \arg [in]end page 포인터의 마지막 주소 값
	 * \arg [in]node nodeid
	 */

	unsigned long addr = start;
	unsigned long next;
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;

	do {
		next = pmd_addr_end(addr, end);							// 다음 pmd 엔트리가 관리할 vmemmap 가상 시작 주소를 구한다. (4K Page 기준 End 가 2MB 보다 클 경우 addr + 2MB 반환, 작을 경우 2MB 반환)

		pgd = vmemmap_pgd_populate(addr, node);					// 1 Page allocation 진행 이후 Pgd table 등록
		if (!pgd)
			return -ENOMEM;

		pud = vmemmap_pud_populate(pgd, addr, node);			// pgd_populate와 동일한 시퀀스를 pud 에서 진행
		if (!pud)
			return -ENOMEM;

		pmd = pmd_offset(pud, addr);
		if (pmd_none(*pmd)) {
			void *p = NULL;

			p = vmemmap_alloc_block_buf(PMD_SIZE, node);		// 미리 할당 받은 메모리 중 하나를 가져옴 (없을 경우 새로 할당 받음)
			if (!p)
				return -ENOMEM;

			set_pmd(pmd, __pmd(__pa(p) | PROT_SECT_NORMAL));	// pmd set
		} else
			vmemmap_verify((pte_t *)pmd, node, addr, next);
	} while (addr = next, addr != end);

	return 0;
}
#endif	/* CONFIG_ARM64_64K_PAGES */
void vmemmap_free(unsigned long start, unsigned long end)
{
}
#endif	/* CONFIG_SPARSEMEM_VMEMMAP */

static inline pud_t * fixmap_pud(unsigned long addr)
{
	pgd_t *pgd = pgd_offset_k(addr); // addr의 pgd 주소를 가져옴

	BUG_ON(pgd_none(*pgd) || pgd_bad(*pgd));

	return pud_offset_kimg(pgd, addr); // 매크로 조건문이 달라 pud를 가져오지 않음 (pgd 바로 리턴)
}

static inline pmd_t * fixmap_pmd(unsigned long addr)
{
	pud_t *pud = fixmap_pud(addr);					// addr 의 pud 주소를 가져옴

	BUG_ON(pud_none(*pud) || pud_bad(*pud));

	return pmd_offset_kimg(pud, addr);				// pmd 주소를 리턴
}

static inline pte_t * fixmap_pte(unsigned long addr)
{
	return &bm_pte[pte_index(addr)];
}

void __init early_fixmap_init(void)
{
	// 4 Table mapping
	// pgd -> bm_pud -> bm_pmd -> bm_pte
	// 3 Table mapping
	// pgd -> bm_pmd -> bm_pte
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	unsigned long addr = FIXADDR_START;

	pgd = pgd_offset_k(addr); // addr에 대한 커널용 pgd 엔트리 주소 리턴
	if (CONFIG_PGTABLE_LEVELS > 3 &&										// pud가 필요한 경우 (PGTABLE 4 이상)
	    !(pgd_none(*pgd) || pgd_page_paddr(*pgd) == __pa(bm_pud))) {		// Paging Size 확인 (16K Page 이상)
		/*
		 * We only end up here if the kernel mapping and the fixmap
		 * share the top level pgd entry, which should only happen on
		 * 16k/4 levels configurations.
		 */
		BUG_ON(!IS_ENABLED(CONFIG_ARM64_16K_PAGES));
		pud = pud_offset_kimg(pgd, addr);									// 커널 이미지용 pud 엔트리 주소를 리턴
	} else {																// pud가 없는 경우
		pgd_populate(&init_mm, pgd, bm_pud);								// pgd 엔트리에 bm_pud 주소를 지정
		pud = fixmap_pud(addr);												// pgd 주소를 리턴
	}
	pud_populate(&init_mm, pud, bm_pmd);									// pud 엔트리에 bm_pmd 주소를 지정
	pmd = fixmap_pmd(addr);													// pmd 주소를 리턴
	pmd_populate_kernel(&init_mm, pmd, bm_pte);								// pmd 엔트리에 bm_pte 주소를 지정

	/*
	 * The boot-ioremap range spans multiple pmds, for which
	 * we are not prepared:
	 */
	BUILD_BUG_ON((__fix_to_virt(FIX_BTMAP_BEGIN) >> PMD_SHIFT)
		     != (__fix_to_virt(FIX_BTMAP_END) >> PMD_SHIFT));

	if ((pmd != fixmap_pmd(fix_to_virt(FIX_BTMAP_BEGIN)))					// pmd 엔트리 하나가 fixmap 영역 전체를 커버하지 못하는 경우
	     || pmd != fixmap_pmd(fix_to_virt(FIX_BTMAP_END))) {
		WARN_ON(1);
		pr_warn("pmd %p != %p, %p\n",
			pmd, fixmap_pmd(fix_to_virt(FIX_BTMAP_BEGIN)),
			fixmap_pmd(fix_to_virt(FIX_BTMAP_END)));
		pr_warn("fix_to_virt(FIX_BTMAP_BEGIN): %08lx\n",
			fix_to_virt(FIX_BTMAP_BEGIN));
		pr_warn("fix_to_virt(FIX_BTMAP_END):   %08lx\n",
			fix_to_virt(FIX_BTMAP_END));

		pr_warn("FIX_BTMAP_END:       %d\n", FIX_BTMAP_END);
		pr_warn("FIX_BTMAP_BEGIN:     %d\n", FIX_BTMAP_BEGIN);
	}
}

void __set_fixmap(enum fixed_addresses idx,
			       phys_addr_t phys, pgprot_t flags)
{
	unsigned long addr = __fix_to_virt(idx); 						// idx에 해당하는 fixmap 주소를 리턴
	pte_t *pte;

	BUG_ON(idx <= FIX_HOLE || idx >= __end_of_fixed_addresses); 	// 메모리가 fixmap 영역안에 있는 지 확인 

	pte = fixmap_pte(addr);											// idx 에 해당하는 pte 주소를 리턴

	if (pgprot_val(flags)) {										// pgprot을 사용할 경우 flags.pgprot 리턴 아닌 경우 flags 리턴
		set_pte(pte, pfn_pte(phys >> PAGE_SHIFT, flags));			// pte에 물리 메모리 매핑
	} else {
		pte_clear(&init_mm, addr, pte);								// 물리 메모리 언매핑
		flush_tlb_kernel_range(addr, addr+PAGE_SIZE);				// cache clear
	}
}

void *__init __fixmap_remap_fdt(phys_addr_t dt_phys, int *size, pgprot_t prot)
{
	const u64 dt_virt_base = __fix_to_virt(FIX_FDT);
	int offset;
	void *dt_virt;

	/*
	 * Check whether the physical FDT address is set and meets the minimum
	 * alignment requirement. Since we are relying on MIN_FDT_ALIGN to be
	 * at least 8 bytes so that we can always access the size field of the
	 * FDT header after mapping the first chunk, double check here if that
	 * is indeed the case.
	 */
	BUILD_BUG_ON(MIN_FDT_ALIGN < 8);
	if (!dt_phys || dt_phys % MIN_FDT_ALIGN)
		return NULL;

	/*
	 * Make sure that the FDT region can be mapped without the need to
	 * allocate additional translation table pages, so that it is safe
	 * to call create_mapping_noalloc() this early.
	 *
	 * On 64k pages, the FDT will be mapped using PTEs, so we need to
	 * be in the same PMD as the rest of the fixmap.
	 * On 4k pages, we'll use section mappings for the FDT so we only
	 * have to be in the same PUD.
	 */
	BUILD_BUG_ON(dt_virt_base % SZ_2M);

	BUILD_BUG_ON(__fix_to_virt(FIX_FDT_END) >> SWAPPER_TABLE_SHIFT !=
		     __fix_to_virt(FIX_BTMAP_BEGIN) >> SWAPPER_TABLE_SHIFT);

	offset = dt_phys % SWAPPER_BLOCK_SIZE;
	dt_virt = (void *)dt_virt_base + offset;

	/* map the first chunk so we can read the size from the header */
	create_mapping_noalloc(round_down(dt_phys, SWAPPER_BLOCK_SIZE),
			dt_virt_base, SWAPPER_BLOCK_SIZE, prot);

	if (fdt_check_header(dt_virt) != 0)
		return NULL;

	*size = fdt_totalsize(dt_virt);
	if (*size > MAX_FDT_SIZE)
		return NULL;

	if (offset + *size > SWAPPER_BLOCK_SIZE)
		create_mapping_noalloc(round_down(dt_phys, SWAPPER_BLOCK_SIZE), dt_virt_base,
			       round_up(offset + *size, SWAPPER_BLOCK_SIZE), prot);

	return dt_virt;
}

void *__init fixmap_remap_fdt(phys_addr_t dt_phys)
{
	void *dt_virt;
	int size;

	dt_virt = __fixmap_remap_fdt(dt_phys, &size, PAGE_KERNEL_RO);
	if (!dt_virt)
		return NULL;

	memblock_reserve(dt_phys, size);
	return dt_virt;
}

int __init arch_ioremap_pud_supported(void)
{
	/* only 4k granule supports level 1 block mappings */
	return IS_ENABLED(CONFIG_ARM64_4K_PAGES);
}

int __init arch_ioremap_pmd_supported(void)
{
	return 1;
}

int pud_set_huge(pud_t *pud, phys_addr_t phys, pgprot_t prot)
{
	BUG_ON(phys & ~PUD_MASK);
	set_pud(pud, __pud(phys | PUD_TYPE_SECT | pgprot_val(mk_sect_prot(prot))));
	return 1;
}

int pmd_set_huge(pmd_t *pmd, phys_addr_t phys, pgprot_t prot)
{
	BUG_ON(phys & ~PMD_MASK);
	set_pmd(pmd, __pmd(phys | PMD_TYPE_SECT | pgprot_val(mk_sect_prot(prot))));
	return 1;
}

int pud_clear_huge(pud_t *pud)
{
	if (!pud_sect(*pud))
		return 0;
	pud_clear(pud);
	return 1;
}

int pmd_clear_huge(pmd_t *pmd)
{
	if (!pmd_sect(*pmd))
		return 0;
	pmd_clear(pmd);
	return 1;
}
