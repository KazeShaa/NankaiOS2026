#include <pmm.h>
#include <list.h>
#include <buddy_system_pmm.h>
#include <stdio.h>

// 伙伴系统（Buddy System）分配器
//
// 用 MAX_ORDER+1 个空闲链表 free_area[order] 组织空闲块：free_area[k] 存放
// "大小恰为 2^k 页"的块，块头页的 property 记录该块大小（页数），并置
// PageProperty 标志表示这是一个空闲块头。
//
// 2^14 = 16384 页 = 64MB。物理内存共 128MB，去掉 OpenSBI/内核/描述符占用后，
// 最大的空闲块不超过 64MB，因此 MAX_ORDER = 14 已足够。
#define MAX_ORDER 14

static free_area_t free_area[MAX_ORDER + 1];
// 总空闲页数
static size_t nr_free;

// 把 n 向上取整到 2 的幂，返回对应的阶数（即 2^order >= n）
static inline int
size_to_order(size_t n) {
    int order = 0;
    size_t size = 1;
    while (size < n) {
        size <<= 1;
        order++;
    }
    return order;
}

static void
buddy_init(void) {
    for (int i = 0; i <= MAX_ORDER; i++) {
        list_init(&free_area[i].free_list);
    }
    nr_free = 0;
}

static void
buddy_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    // 先清空这段连续页的描述符（这些页应已由 page_init 标记为 reserved）
    struct Page *p = base;
    for (; p != base + n; p++) {
        assert(PageReserved(p));
        p->flags = 0;
        set_page_ref(p, 0);
        p->property = 0;
    }

    // 把 [base, base+n) 拆成若干"对齐到自身大小"的 2 的幂块，分别挂到对应链表
    size_t idx = base - pages;
    size_t end = idx + n;
    while (idx < end) {
        size_t remain = end - idx;
        int order = 0;
        size_t size = 1;
        // 找最大的 order，使 size=2^order <= remain 且 idx 对齐到 size
        while (order + 1 <= MAX_ORDER && (size << 1) <= remain &&
               (idx & ((size << 1) - 1)) == 0) {
            order++;
            size <<= 1;
        }
        struct Page *blk = pages + idx;
        blk->property = size;
        SetPageProperty(blk);
        list_add(&free_area[order].free_list, &(blk->page_link));
        nr_free += size;
        idx += size;
    }
}

static struct Page *
buddy_alloc_pages(size_t n) {
    assert(n > 0);
    if (n > nr_free) {
        return NULL;
    }
    int order = size_to_order(n);
    if (order > MAX_ORDER) {
        return NULL;  // 请求的块比最大可表示的块还大
    }
    // 从 order 向上找第一个非空链表
    int cur = order;
    while (cur <= MAX_ORDER && list_empty(&free_area[cur].free_list)) {
        cur++;
    }
    if (cur > MAX_ORDER) {
        return NULL;  // 空闲页总数够，但没有足够大的连续块
    }
    // 取出 free_area[cur] 的一个块
    list_entry_t *le = list_next(&free_area[cur].free_list);
    struct Page *page = le2page(le, page_link);
    list_del(&(page->page_link));
    // 一路向下劈开到目标 order，高地址一半挂回链表
    while (cur > order) {
        cur--;
        struct Page *buddy = page + (1 << cur);
        buddy->property = 1 << cur;
        SetPageProperty(buddy);
        list_add(&free_area[cur].free_list, &(buddy->page_link));
    }
    page->property = 1 << order;
    ClearPageProperty(page);
    nr_free -= (1 << order);
    return page;
}

static void
buddy_free_pages(struct Page *base, size_t n) {
    assert(n > 0);
    int order = size_to_order(n);
    size_t size = 1 << order;

    // 清空被释放页的描述符
    struct Page *p = base;
    for (; p != base + size; p++) {
        assert(!PageReserved(p) && !PageProperty(p));
        p->flags = 0;
        set_page_ref(p, 0);
        p->property = 0;
    }

    size_t idx = base - pages;
    // 逐级向上合并：伙伴地址 = 自身地址 XOR 块大小
    while (order < MAX_ORDER) {
        size_t buddy_idx = idx ^ ((size_t)1 << order);
        if (buddy_idx >= (size_t)(npage - nbase)) {
            break;  // 伙伴越界（对应保留区，不可能合并）
        }
        struct Page *buddy = pages + buddy_idx;
        if (!PageProperty(buddy) || buddy->property != ((size_t)1 << order)) {
            break;  // 伙伴不是同阶空闲块
        }
        // 摘下伙伴，与当前块合并
        list_del(&(buddy->page_link));
        ClearPageProperty(buddy);
        buddy->property = 0;
        idx = idx & ~((size_t)1 << order);  // 合并后块头是低地址那一半
        order++;
    }

    base = pages + idx;
    base->property = 1 << order;
    SetPageProperty(base);
    list_add(&free_area[order].free_list, &(base->page_link));
    nr_free += size;
}

static size_t
buddy_nr_free_pages(void) {
    return nr_free;
}

// 自测：验证不变量与各类分配/释放往返
static void
buddy_check(void) {
    // (1) 不变量：每块 property == 2^order，且所有块大小之和 == nr_free
    size_t total = 0;
    for (int order = 0; order <= MAX_ORDER; order++) {
        list_entry_t *le = &free_area[order].free_list;
        while ((le = list_next(le)) != &free_area[order].free_list) {
            struct Page *p = le2page(le, page_link);
            assert(PageProperty(p));
            assert(p->property == (size_t)1 << order);
            total += p->property;
        }
    }
    assert(total == nr_free);

    // (2) 单页往返
    struct Page *p0 = alloc_page();
    struct Page *p1 = alloc_page();
    struct Page *p2 = alloc_page();
    assert(p0 != NULL && p1 != NULL && p2 != NULL);
    assert(p0 != p1 && p0 != p2 && p1 != p2);
    assert(!PageProperty(p0) && !PageProperty(p1) && !PageProperty(p2));
    assert(nr_free == total - 3);
    free_page(p0);
    free_page(p1);
    free_page(p2);
    assert(nr_free == total);

    // (3) 2 的幂块往返
    struct Page *p8 = alloc_pages(8);
    assert(p8 != NULL);
    assert(nr_free == total - 8);
    free_pages(p8, 8);
    assert(nr_free == total);

    // (4) 非 2 的幂向上取整
    struct Page *p5 = alloc_pages(5);
    assert(p5 != NULL);
    assert(nr_free == total - 8);  // 5 页实际占 8 页
    free_pages(p5, 5);
    assert(nr_free == total);

    // (5) 最大可用块往返
    int max_order = -1;
    for (int order = MAX_ORDER; order >= 0; order--) {
        if (!list_empty(&free_area[order].free_list)) {
            max_order = order;
            break;
        }
    }
    assert(max_order >= 0);
    size_t max_size = (size_t)1 << max_order;
    struct Page *pm = alloc_pages(max_size);
    assert(pm != NULL);
    assert(nr_free == total - max_size);
    free_pages(pm, max_size);
    assert(nr_free == total);

    // (6) 结束时再次检查不变量
    total = 0;
    for (int order = 0; order <= MAX_ORDER; order++) {
        list_entry_t *le = &free_area[order].free_list;
        while ((le = list_next(le)) != &free_area[order].free_list) {
            struct Page *p = le2page(le, page_link);
            assert(PageProperty(p));
            assert(p->property == (size_t)1 << order);
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
