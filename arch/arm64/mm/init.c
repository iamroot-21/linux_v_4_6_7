/*
 * Based on arch/arm/mm/init.c
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

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/errno.h>
#include <linux/swap.h>
#include <linux/init.h>
#include <linux/bootmem.h>
#include <linux/mman.h>
#include <linux/nodemask.h>
#include <linux/initrd.h>
#include <linux/gfp.h>
#include <linux/memblock.h>
#include <linux/sort.h>
#include <linux/of_fdt.h>
#include <linux/dma-mapping.h>
#include <linux/dma-contiguous.h>
#include <linux/efi.h>
#include <linux/swiotlb.h>

#include <asm/boot.h>
#include <asm/fixmap.h>
#include <asm/kasan.h>
#include <asm/kernel-pgtable.h>
#include <asm/memory.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/sizes.h>
#include <asm/tlb.h>
#include <asm/alternative.h>

#include "mm.h"

/*
 * We need to be able to catch inadvertent references to memstart_addr
 * that occur (potentially in generic code) before arm64_memblock_init()
 * executes, which assigns it its actual value. So use a default value
 * that cannot be mistaken for a real physical address.
 */
s64 memstart_addr __read_mostly = -1;
phys_addr_t arm64_dma_phys_limit __read_mostly;

#ifdef CONFIG_BLK_DEV_INITRD
static int __init early_initrd(char *p)
{
	unsigned long start, size;
	char *endp;

	start = memparse(p, &endp);
	if (*endp == ',') {
		size = memparse(endp + 1, NULL);

		initrd_start = start;
		initrd_end = start + size;
	}
	return 0;
}
early_param("initrd", early_initrd);
#endif

/*
 * Return the maximum physical address for ZONE_DMA (DMA_BIT_MASK(32)). It
 * currently assumes that for memory starting above 4G, 32-bit devices will
 * use a DMA offset.
 */
static phys_addr_t __init max_zone_dma_phys(void)
{
	phys_addr_t offset = memblock_start_of_DRAM() & GENMASK_ULL(63, 32);
	return min(offset + (1ULL << 32), memblock_end_of_DRAM());
}

static void __init zone_sizes_init(unsigned long min, unsigned long max)
{
	/**
	 * \args[in] min Page frame number(memblock_start_of_DRAM)
	 * \args[in] max PaGE frame number(memblock_end_of_DRAM)
	 */
	struct memblock_region *reg;
	unsigned long zone_size[MAX_NR_ZONES], zhole_size[MAX_NR_ZONES];		// size 저장할 array 선언
	unsigned long max_dma = min;

	memset(zone_size, 0, sizeof(zone_size));

	/* 4GB maximum for 32-bit only capable devices */
#define CONFIG_ZONE_DMA														// 임시 선언
#ifdef CONFIG_ZONE_DMA
	max_dma = PFN_DOWN(arm64_dma_phys_limit);							
	zone_size[ZONE_DMA] = max_dma - min;									// dma max - min (남아있는 dma size)
#endif
	zone_size[ZONE_NORMAL] = max - max_dma;									// max - min, DMA 사용 시, max - DMA영역 사이즈

	memcpy(zhole_size, zone_size, sizeof(zhole_size));

	for_each_memblock(memory, reg) {
		unsigned long start = memblock_region_memory_base_pfn(reg);			// start region -> page frame number
		unsigned long end = memblock_region_memory_end_pfn(reg);			// end region -> page frame number

		if (start >= max)
			continue;

#ifdef CONFIG_ZONE_DMA
		if (start < max_dma) {												// dma hole이 남아있을 경우
			unsigned long dma_end = min(end, max_dma);
			zhole_size[ZONE_DMA] -= dma_end - start;						// dma hole 업데이트
		}
#endif
		if (end > max_dma) {												// dma hole이 남아있지 않을 경우
			unsigned long normal_end = min(end, max);
			unsigned long normal_start = max(start, max_dma);
			zhole_size[ZONE_NORMAL] -= normal_end - normal_start;			// zone hole 업데이트
		}
	}

	free_area_init_node(0, zone_size, min, zhole_size);
#undef CONFIG_ZONE_DMA															// 임시 선언
}

#ifdef CONFIG_HAVE_ARCH_PFN_VALID
int pfn_valid(unsigned long pfn)
{
	return memblock_is_map_memory(pfn << PAGE_SHIFT);
}
EXPORT_SYMBOL(pfn_valid);
#endif

#define CONFIG_SPARSEMEM
#ifndef CONFIG_SPARSEMEM
static void __init arm64_memory_present(void)
{
}
#else
static void __init arm64_memory_present(void)
{
	struct memblock_region *reg;

	for_each_memblock(memory, reg)
		memory_present(0, memblock_region_memory_base_pfn(reg),
			       memblock_region_memory_end_pfn(reg));
}
#endif

static phys_addr_t memory_limit = (phys_addr_t)ULLONG_MAX;

/*
 * Limit the memory size that was specified via FDT.
 */
static int __init early_mem(char *p)
{
	if (!p)
		return 1;

	memory_limit = memparse(p, &p) & PAGE_MASK;
	pr_notice("Memory limited to %lldMB\n", memory_limit >> 20);

	return 0;
}
early_param("mem", early_mem);

void __init arm64_memblock_init(void)
{
	const s64 linear_region_size = -(s64)PAGE_OFFSET;								//  1 << (39-1)

	/*
	 * Ensure that the linear region takes up exactly half of the kernel
	 * virtual address space. This way, we can distinguish a linear address
	 * from a kernel/module/vmalloc address by testing a single bit.
	 */
	BUILD_BUG_ON(linear_region_size != BIT(VA_BITS - 1));							// 1 << (39 -1) != (VA_BITS -1) 

	/*
	 * Select a suitable value for the base of physical memory.
	 */
	memstart_addr = round_down(memblock_start_of_DRAM(),							// 설정에 맞춰서 PUD 크기로 정렬하여 사용
				   ARM64_MEMSTART_ALIGN);

	/*
	 * Remove the memory that we will not be able to cover with the
	 * linear mapping. Take care not to clip the kernel which may be
	 * high in memory.
	 */
	memblock_remove(max_t(u64, memstart_addr + linear_region_size, __pa(_end)),		// memstart 부터 linear_region_size 를 초과하는 지점의 memblock 제거
			ULLONG_MAX);
	if (memblock_end_of_DRAM() > linear_region_size)								// 물리 메모리가 256GB 보다 큰 경우 그 이상의 memblock 제거
		memblock_remove(0, memblock_end_of_DRAM() - linear_region_size);

	/*
	 * Apply the memory limit if it was set. Since the kernel may be loaded
	 * high up in memory, add back the kernel region that must be accessible
	 * via the linear mapping.
	 */
	if (memory_limit != (phys_addr_t)ULLONG_MAX) {									// memory_limit 가 설정이 변경된 경우
		memblock_enforce_memory_limit(memory_limit);1								// 루프를 순회하여 메모리 영역 계산, 초과하는 memblock 제거
		memblock_add(__pa(_text), (u64)(_end - _text));								// Kernel 영역 만큼의 memblock 생성
	}

	if (IS_ENABLED(CONFIG_RANDOMIZE_BASE)) {										// Randomize
		extern u16 memstart_offset_seed;
		u64 range = linear_region_size - 											// region_size - total_memblock_size
			    (memblock_end_of_DRAM() - memblock_start_of_DRAM());

		/*
		 * If the size of the linear region exceeds, by a sufficient
		 * margin, the size of the region that the available physical
		 * memory spans, randomize the linear region as well.
		 */
		if (memstart_offset_seed > 0 && range >= ARM64_MEMSTART_ALIGN) {
			range = range / ARM64_MEMSTART_ALIGN + 1;
			memstart_addr -= ARM64_MEMSTART_ALIGN *									// memstart 지점을 랜덤하게 변경
					 ((range * memstart_offset_seed) >> 16);
		}
	}

	/*
	 * Register the kernel text, kernel data, initrd, and initial
	 * pagetables with memblock.
	 */
	memblock_reserve(__pa(_text), _end - _text);									// Kernel 영역 reserve
#ifdef CONFIG_BLK_DEV_INITRD
	if (initrd_start) {																// Ramdisk 를 가상 주소로 변환하여 할당
		memblock_reserve(initrd_start, initrd_end - initrd_start);

		/* the generic initrd code expects virtual addresses */
		initrd_start = __phys_to_virt(initrd_start);
		initrd_end = __phys_to_virt(initrd_end);
	}
#endif


	early_init_fdt_scan_reserved_mem();												// TODO 분석 필요, FDT 에서 세 가지 영역 추가 (DTB 영역, DTB 헤더의 memory reserve 블록 영역, DTB reserved-mem 노드 영역)

	/* 4GB maximum for 32-bit only capable devices */
	if (IS_ENABLED(CONFIG_ZONE_DMA))
		arm64_dma_phys_limit = max_zone_dma_phys();									// TODO 분석 필요 (DMA Zone 설정)
	else
		arm64_dma_phys_limit = PHYS_MASK + 1;
	dma_contiguous_reserve(arm64_dma_phys_limit);									// TODO 분석 필요

	memblock_allow_resize();														// resize 허용 플래그 설정
	memblock_dump_all();															// 디버깅 코드
}

void __init bootmem_init(void)
{
	unsigned long min, max;

	// TODO PFN_UP, PFN DOWN 사용 이유 확인 필요
	min = PFN_UP(memblock_start_of_DRAM());											// memblock index = 0, base 값의 페이지 프레임 넘버를 가져옴
	max = PFN_DOWN(memblock_end_of_DRAM());											// memblock 마지막 index의 end 값의 페이지 프레임 넘버를 가져옴

	early_memtest(min << PAGE_SHIFT, max << PAGE_SHIFT);							// min, max 값에 memtest_pattern 에서 설정한 값을 써서 실제로 써지는 지 확인
																					// 값이 안써지는 경우 bad mem으로 reserve
	/*
	 * Sparsemem tries to allocate bootmem in memory_present(), so must be
	 * done after the fixed reservations.
	 */
	arm64_memory_present();															// mem_section allocation

	sparse_init();																	// sparse memory model 일 경우, 초기화 진행
	zone_sizes_init(min, max);														// zone 별로 메모리 영역 초기화

	high_memory = __va((max << PAGE_SHIFT) - 1) + 1;
	max_pfn = max_low_pfn = max;
}

#ifndef CONFIG_SPARSEMEM_VMEMMAP
static inline void free_memmap(unsigned long start_pfn, unsigned long end_pfn)
{
	struct page *start_pg, *end_pg;
	unsigned long pg, pgend;

	/*
	 * Convert start_pfn/end_pfn to a struct page pointer.
	 */
	start_pg = pfn_to_page(start_pfn - 1) + 1;
	end_pg = pfn_to_page(end_pfn - 1) + 1;

	/*
	 * Convert to physical addresses, and round start upwards and end
	 * downwards.
	 */
	pg = (unsigned long)PAGE_ALIGN(__pa(start_pg));
	pgend = (unsigned long)__pa(end_pg) & PAGE_MASK;

	/*
	 * If there are free pages between these, free the section of the
	 * memmap array.
	 */
	if (pg < pgend)
		free_bootmem(pg, pgend - pg);
}

/*
 * The mem_map array can get very big. Free the unused area of the memory map.
 */
static void __init free_unused_memmap(void)
{
	unsigned long start, prev_end = 0;
	struct memblock_region *reg;

	for_each_memblock(memory, reg) {
		start = __phys_to_pfn(reg->base);

#ifdef CONFIG_SPARSEMEM
		/*
		 * Take care not to free memmap entries that don't exist due
		 * to SPARSEMEM sections which aren't present.
		 */
		start = min(start, ALIGN(prev_end, PAGES_PER_SECTION));
#endif
		/*
		 * If we had a previous bank, and there is a space between the
		 * current bank and the previous, free it.
		 */
		if (prev_end && prev_end < start)
			free_memmap(prev_end, start);

		/*
		 * Align up here since the VM subsystem insists that the
		 * memmap entries are valid from the bank end aligned to
		 * MAX_ORDER_NR_PAGES.
		 */
		prev_end = ALIGN(__phys_to_pfn(reg->base + reg->size),
				 MAX_ORDER_NR_PAGES);
	}

#ifdef CONFIG_SPARSEMEM
	if (!IS_ALIGNED(prev_end, PAGES_PER_SECTION))
		free_memmap(prev_end, ALIGN(prev_end, PAGES_PER_SECTION));
#endif
}
#endif	/* !CONFIG_SPARSEMEM_VMEMMAP */

/*
 * mem_init() marks the free areas in the mem_map and tells us how much memory
 * is free.  This is done after various parts of the system have claimed their
 * memory after the kernel image.
 */
void __init mem_init(void)
{
	swiotlb_init(1);

	set_max_mapnr(pfn_to_page(max_pfn) - mem_map);

#ifndef CONFIG_SPARSEMEM_VMEMMAP
	free_unused_memmap();
#endif
	/* this will put all unused low memory onto the freelists */
	free_all_bootmem();

	mem_init_print_info(NULL);

#define MLK(b, t) b, t, ((t) - (b)) >> 10
#define MLM(b, t) b, t, ((t) - (b)) >> 20
#define MLG(b, t) b, t, ((t) - (b)) >> 30
#define MLK_ROUNDUP(b, t) b, t, DIV_ROUND_UP(((t) - (b)), SZ_1K)

	pr_notice("Virtual kernel memory layout:\n");
#ifdef CONFIG_KASAN
	pr_cont("    kasan   : 0x%16lx - 0x%16lx   (%6ld GB)\n",
		MLG(KASAN_SHADOW_START, KASAN_SHADOW_END));
#endif
	pr_cont("    modules : 0x%16lx - 0x%16lx   (%6ld MB)\n",
		MLM(MODULES_VADDR, MODULES_END));
	pr_cont("    vmalloc : 0x%16lx - 0x%16lx   (%6ld GB)\n",
		MLG(VMALLOC_START, VMALLOC_END));
	pr_cont("      .text : 0x%p" " - 0x%p" "   (%6ld KB)\n"
		"    .rodata : 0x%p" " - 0x%p" "   (%6ld KB)\n"
		"      .init : 0x%p" " - 0x%p" "   (%6ld KB)\n"
		"      .data : 0x%p" " - 0x%p" "   (%6ld KB)\n",
		MLK_ROUNDUP(_text, __start_rodata),
		MLK_ROUNDUP(__start_rodata, _etext),
		MLK_ROUNDUP(__init_begin, __init_end),
		MLK_ROUNDUP(_sdata, _edata));
#ifdef CONFIG_SPARSEMEM_VMEMMAP
	pr_cont("    vmemmap : 0x%16lx - 0x%16lx   (%6ld GB maximum)\n"
		"              0x%16lx - 0x%16lx   (%6ld MB actual)\n",
		MLG(VMEMMAP_START,
		    VMEMMAP_START + VMEMMAP_SIZE),
		MLM((unsigned long)phys_to_page(memblock_start_of_DRAM()),
		    (unsigned long)virt_to_page(high_memory)));
#endif
	pr_cont("    fixed   : 0x%16lx - 0x%16lx   (%6ld KB)\n",
		MLK(FIXADDR_START, FIXADDR_TOP));
	pr_cont("    PCI I/O : 0x%16lx - 0x%16lx   (%6ld MB)\n",
		MLM(PCI_IO_START, PCI_IO_END));
	pr_cont("    memory  : 0x%16lx - 0x%16lx   (%6ld MB)\n",
		MLM(__phys_to_virt(memblock_start_of_DRAM()),
		    (unsigned long)high_memory));

#undef MLK
#undef MLM
#undef MLK_ROUNDUP

	/*
	 * Check boundaries twice: Some fundamental inconsistencies can be
	 * detected at build time already.
	 */
#ifdef CONFIG_COMPAT
	BUILD_BUG_ON(TASK_SIZE_32			> TASK_SIZE_64);
#endif

	if (PAGE_SIZE >= 16384 && get_num_physpages() <= 128) {
		extern int sysctl_overcommit_memory;
		/*
		 * On a machine this small we won't get anywhere without
		 * overcommit, so turn it on by default.
		 */
		sysctl_overcommit_memory = OVERCOMMIT_ALWAYS;
	}
}

void free_initmem(void)
{
	free_initmem_default(0);
	fixup_init();
}

#ifdef CONFIG_BLK_DEV_INITRD

static int keep_initrd __initdata;

void __init free_initrd_mem(unsigned long start, unsigned long end)
{
	if (!keep_initrd)
		free_reserved_area((void *)start, (void *)end, 0, "initrd");
}

static int __init keepinitrd_setup(char *__unused)
{
	keep_initrd = 1;
	return 1;
}

__setup("keepinitrd", keepinitrd_setup);
#endif

/*
 * Dump out memory limit information on panic.
 */
static int dump_mem_limit(struct notifier_block *self, unsigned long v, void *p)
{
	if (memory_limit != (phys_addr_t)ULLONG_MAX) {
		pr_emerg("Memory Limit: %llu MB\n", memory_limit >> 20);
	} else {
		pr_emerg("Memory Limit: none\n");
	}
	return 0;
}

static struct notifier_block mem_limit_notifier = {
	.notifier_call = dump_mem_limit,
};

static int __init register_mem_limit_dumper(void)
{
	atomic_notifier_chain_register(&panic_notifier_list,
				       &mem_limit_notifier);
	return 0;
}
__initcall(register_mem_limit_dumper);
