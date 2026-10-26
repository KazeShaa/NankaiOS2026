#include <default_pmm.h>
#include <best_fit_pmm.h>
#include <buddy_system_pmm.h>
#include <defs.h>
#include <error.h>
#include <memlayout.h>
#include <mmu.h>
#include <pmm.h>
#include <sbi.h>
#include <stdio.h>
#include <string.h>
#include <riscv.h>
#include <dtb.h>

// virtual address of physical page array
struct Page *pages;
// amount of physical memory (in pages)
size_t npage = 0;
// the kernel image is mapped at VA=KERNBASE and PA=info.base
uint64_t va_pa_offset;
// memory starts at 0x80000000 in RISC-V
// DRAM_BASE defined in riscv.h as 0x80000000
const size_t nbase = DRAM_BASE / PGSIZE;

// virtual address of boot-time page directory
uintptr_t *satp_virtual = NULL;
// physical address of boot-time page directory
uintptr_t satp_physical;

// physical memory management
const struct pmm_manager *pmm_manager;


static void check_alloc_page(void);

// init_pmm_manager - initialize a pmm_manager instance
static void init_pmm_manager(void) {
    pmm_manager = &best_fit_pmm_manager;
    cprintf("memory management: %s\n", pmm_manager->name);
    pmm_manager->init();
}

// init_memmap - call pmm->init_memmap to build Page struct for free memory
static void init_memmap(struct Page *base, size_t n) {
    pmm_manager->init_memmap(base, n);
}

// alloc_pages - call pmm->alloc_pages to allocate a continuous n*PAGESIZE
// memory
struct Page *alloc_pages(size_t n) {
    return pmm_manager->alloc_pages(n);
}

// free_pages - call pmm->free_pages to free a continuous n*PAGESIZE memory
void free_pages(struct Page *base, size_t n) {
    pmm_manager->free_pages(base, n);
}

// nr_free_pages - call pmm->nr_free_pages to get the size (nr*PAGESIZE)
// of current free memory
size_t nr_free_pages(void) {
    return pmm_manager->nr_free_pages();
}

//lab2 function1 2412350
// page_init - detect physical memory space, reserve already used memory,
  // then use pmm->init_memmap to create the free page list
  static void
  page_init(void) {
      extern char end[];

      // 内核虚拟地址与物理地址之间的固定偏移
      va_pa_offset = PHYSICAL_MEMORY_OFFSET;

      // 从 DTB 解析结果读取物理内存范围
      uint64_t mem_base = get_memory_base();
      uint64_t mem_size = get_memory_size();

      // (新增1) 设备树信息无效时应终止初始化
      if (mem_base == 0 || mem_size == 0) {
          panic("page_init: invalid physical memory info from DTB");
      }

      uintptr_t mem_end = mem_base + mem_size;

      // (新增2) 输出探测到的物理内存范围
      cprintf("physical memory: 0x%016lx - 0x%016lx (%lu MB)\n",
              mem_base, mem_end - 1, mem_size / (1024 * 1024));

      // 物理页总数：覆盖 [0, mem_end)，含 DRAM_BASE 以下的"虚页"(用 nbase 偏移)
      npage = PPN(mem_end);

      // Page 描述符数组放在内核 bss(end) 之后，向上取整到页边界
      pages = (struct Page *)ROUNDUP((void *)end, PGSIZE);

      // 先把所有页标记为 reserved
      for (size_t i = 0; i < npage; i++) {
          SetPageReserved(pages + i);
      }

      // 空闲内存从 Page 描述符数组之后开始
      uintptr_t freemem = PADDR((uintptr_t)pages + sizeof(struct Page) * npage);
      uintptr_t free_start = ROUNDUP(freemem, PGSIZE);

      // (新增3) 没有可用完整页面时不得调用 init_memmap()
      if (free_start < mem_end) {
          init_memmap(pa2page(free_start), (mem_end - free_start) / PGSIZE);
      }
  }

/* pmm_init - initialize the physical memory management */
void pmm_init(void) {
    // We need to alloc/free the physical memory (granularity is 4KB or other size).
    // So a framework of physical memory manager (struct pmm_manager)is defined in pmm.h
    // First we should init a physical memory manager(pmm) based on the framework.
    // Then pmm can alloc/free the physical memory.
    // Now the first_fit/best_fit/worst_fit/buddy_system pmm are available.
    init_pmm_manager();

    // detect physical memory space, reserve already used memory,
    // then use pmm->init_memmap to create free page list
    page_init();

    // use pmm->check to verify the correctness of the alloc/free function in a pmm
    check_alloc_page();

    extern char boot_page_table_sv39[];
    satp_virtual = (pte_t*)boot_page_table_sv39;
    satp_physical = PADDR(satp_virtual);
    cprintf("satp virtual address: 0x%016lx\nsatp physical address: 0x%016lx\n", satp_virtual, satp_physical);
}

static void check_alloc_page(void) {
    pmm_manager->check();
    cprintf("check_alloc_page() succeeded!\n");
}
