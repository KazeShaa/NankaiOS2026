#include <pmm.h>
#include <list.h>
#include <string.h>
#include <stdio.h>
#include <buddy_system_pmm.h>

// Buddy System（伙伴系统）分配器。
// 把空闲内存组织成大小为 2^order 页的块，order 从 0 到 MAX_ORDER。
// free_list[k] 是"大小恰为 2^k 页"的空闲块链表，块头页用 property 记录块大小(页数)。
// 每个块都对齐到自身大小，伙伴块地址 = 自身地址 XOR 块大小，从而支持二分合并。

#define MAX_ORDER 20   // 2^20 页为理论上限，本实验 128MB(2^15 页)远小于此

static list_entry_t free_list[MAX_ORDER + 1];
static size_t nr_free;

// 页在 pages 数组中的下标（pages[0] 对应 DRAM 基址 0x80000000）
static inline size_t
page_index(struct Page *page) {
    return page - pages;
}

// 满足 2^order >= n 的最小 order
static int
order_of(size_t n) {
    int order = 0;
    size_t size = 1;
    while (size < n) {
        size <<= 1;
        order++;
    }
    return order;
}

// 满足 2^order <= n 的最大 order
static int
floor_order(size_t n) {
    int order = 0;
    while (((size_t)1 << (order + 1)) <= n) {
        order++;
    }
    return order;
}

static void
buddy_init(void) {
    for (int i = 0; i <= MAX_ORDER; i++) {
        list_init(&free_list[i]);
    }
    nr_free = 0;
}

static void
buddy_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    struct Page *p = base;
    for (; p != base + n; p++) {
        assert(PageReserved(p));
        p->flags = p->property = 0;
        set_page_ref(p, 0);
    }
    // 把 [base, base+n) 拆成若干"对齐到自身大小"的 2 的幂块，分别挂到对应 order 链表
    p = base;
    size_t remaining = n;
    while (remaining > 0) {
        int k = floor_order(remaining);
        while (k > 0 && page_index(p) % ((size_t)1 << k) != 0) {
            k--;
        }
        p->property = (size_t)1 << k;
        SetPageProperty(p);
        list_add(&free_list[k], &(p->page_link));
        nr_free += (size_t)1 << k;
        p += (size_t)1 << k;
        remaining -= (size_t)1 << k;
    }
}

// 把 order 级空闲块 p 与它的伙伴递归合并，最终挂到合适的 order 链表上
static void
buddy_merge(struct Page *p, int order) {
    size_t idx = page_index(p);
    size_t npages_dram = npage - nbase;
    while (order < MAX_ORDER) {
        size_t buddy_idx = idx ^ ((size_t)1 << order);
        if (buddy_idx >= npages_dram) {
            break;
        }
        struct Page *buddy = &pages[buddy_idx];
        if (!PageProperty(buddy) || buddy->property != (size_t)1 << order) {
            break;
        }
        // 伙伴空闲且大小一致，合并
        list_del(&(buddy->page_link));
        ClearPageProperty(buddy);
        buddy->property = 0;
        idx &= ~((size_t)1 << order);   // 合并后块头是低地址对齐的那半
        order++;
    }
    p = &pages[idx];
    p->property = (size_t)1 << order;
    SetPageProperty(p);
    list_add(&free_list[order], &(p->page_link));
}

static struct Page *
buddy_alloc_pages(size_t n) {
    assert(n > 0);
    if (n > nr_free) {
        return NULL;
    }
    int order = order_of(n);   // 向上取整到 2^order
    if (order > MAX_ORDER) {
        return NULL;
    }
    // 找第一个"order 及更大且非空"的空闲链表
    int o = order;
    while (o <= MAX_ORDER && list_empty(&free_list[o])) {
        o++;
    }
    if (o > MAX_ORDER) {
        return NULL;
    }
    list_entry_t *le = list_next(&free_list[o]);
    list_del(le);
    struct Page *block = le2page(le, page_link);
    // 从大块一路向下劈开，直到得到 2^order 大小的块
    while (o > order) {
        o--;
        size_t half = (size_t)1 << o;
        struct Page *buddy = block + half;   // 高地址那一半挂回链表
        buddy->property = half;
        SetPageProperty(buddy);
        list_add(&free_list[o], &(buddy->page_link));
    }
    ClearPageProperty(block);
    block->property = 0;
    nr_free -= (size_t)1 << order;
    return block;
}

static void
buddy_free_pages(struct Page *base, size_t n) {
    assert(n > 0);
    int order = order_of(n);   // free 同样按 2^order 向上取整，与 alloc 对应
    size_t size = (size_t)1 << order;
    struct Page *p = base;
    for (; p != base + size; p++) {
        assert(!PageReserved(p) && !PageProperty(p));
        p->flags = 0;
        set_page_ref(p, 0);
    }
    nr_free += size;
    buddy_merge(base, order);
}

static size_t
buddy_nr_free_pages(void) {
    return nr_free;
}

// 自测：验证分配/释放/合并的核心行为与边界情况
static void
buddy_check(void) {
    // 0. 不变量：各 order 链表块大小之和 == nr_free
    size_t total = 0;
    for (int k = 0; k <= MAX_ORDER; k++) {
        list_entry_t *le = &free_list[k];
        while ((le = list_next(le)) != &free_list[k]) {
            struct Page *p = le2page(le, page_link);
            assert(PageProperty(p));
            assert(p->property == (size_t)1 << k);
            total += p->property;
        }
    }
    assert(total == nr_free);

    size_t init = nr_free;

    // 1. 单页分配/释放往返
    struct Page *p0 = alloc_page();
    struct Page *p1 = alloc_page();
    struct Page *p2 = alloc_page();
    assert(p0 != NULL && p1 != NULL && p2 != NULL);
    assert(p0 != p1 && p0 != p2 && p1 != p2);
    assert(!PageProperty(p0) && !PageProperty(p1) && !PageProperty(p2));
    assert(nr_free == init - 3);
    free_page(p0);
    free_page(p1);
    free_page(p2);
    assert(nr_free == init);

    // 2. 2 的幂块往返
    struct Page *a = alloc_pages(8);
    assert(a != NULL);
    assert(nr_free == init - 8);
    free_pages(a, 8);
    assert(nr_free == init);

    // 3. 非 2 的幂向上取整（5 -> 8），按相同 n 释放往返
    struct Page *b = alloc_pages(5);
    assert(b != NULL);
    assert(nr_free == init - 8);
    free_pages(b, 5);
    assert(nr_free == init);

    // 4. 最大可用块往返（覆盖一次完整的大块拆分/合并）
    int max_order = MAX_ORDER;
    while (max_order >= 0 && list_empty(&free_list[max_order])) {
        max_order--;
    }
    if (max_order >= 1) {
        size_t big = (size_t)1 << max_order;
        struct Page *bp = alloc_pages(big);
        assert(bp != NULL);
        assert(nr_free == init - big);
        free_pages(bp, big);
        assert(nr_free == init);
    }

    // 5. 结束后不变量再次成立
    total = 0;
    for (int k = 0; k <= MAX_ORDER; k++) {
        list_entry_t *le = &free_list[k];
        while ((le = list_next(le)) != &free_list[k]) {
            struct Page *p = le2page(le, page_link);
            total += p->property;
        }
    }
    assert(total == nr_free);

    cprintf("buddy_check() succeeded!\n");
}

const struct pmm_manager buddy_system_pmm_manager = {
    .name = "buddy_system_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_check,
};
