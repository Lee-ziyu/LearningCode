#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

// ================= 1. 基础配置 =================
#define MEM_SIZE     (128 * 1024 * 1024)
#define PAGE_SHIFT   12
#define PAGE_SIZE    (1UL << PAGE_SHIFT)
#define MAX_ORDER    16

// 模拟物理内存基地址
void *PHYS_MEM_START = NULL;
struct page *MEM_MAP = NULL;

// ================= 2. 核心数据结构 (修正版) =================

#define PG_free      0x01
#define PG_buddy     0x02
#define PG_slab      0x04

struct kmem_cache;

// [FIX] 修正后的 struct page
// 我们将 next 指针移出 union，或者精心安排布局。
// 为了模拟真实内核复用字段的特性，同时避免bug，我们这样修改：
struct page {
    uint32_t flags;

    // 用于链表管理的通用指针 (Buddy用它连空闲页，Slab用连Partial页)
    // 把它移出 Union，避免覆盖关键数据
    struct page *next;

    union {
        // ---用于 Buddy 系统---
        struct {
            int order;
        };

        // ---用于 Slab 系统---
        struct {
            struct kmem_cache *slab_cache;
            void *freelist;
            int active_objects;
        };
    };
};

typedef struct kmem_cache {
    uint32_t obj_size;
    struct page *partial;
} kmem_cache_t;

struct page *buddy_free_area[MAX_ORDER];

#define SLAB_INDEX_COUNT 7
uint32_t slab_sizes[] = {32, 64, 128, 256, 512, 1024, 2048};
kmem_cache_t slab_caches[SLAB_INDEX_COUNT];

// ================= 3. 地址转换 =================

struct page *virt_to_page(void *addr) {
    // [Safety] 检查指针范围
    if (addr < PHYS_MEM_START || (uint8_t*)addr >= (uint8_t*)PHYS_MEM_START + MEM_SIZE)
        return NULL;

    unsigned long offset = (unsigned long)((uint8_t *)addr - (uint8_t *)PHYS_MEM_START);
    unsigned long pfn = offset >> PAGE_SHIFT;
    return &MEM_MAP[pfn];
}

void *page_address(struct page *page) {
    unsigned long pfn = page - MEM_MAP;
    return (uint8_t *)PHYS_MEM_START + (pfn << PAGE_SHIFT);
}

// ================= 4. Buddy System =================

void buddy_init() {
    for (int i = 0; i < MAX_ORDER; i++) buddy_free_area[i] = NULL;

    unsigned long total_pages = MEM_SIZE / PAGE_SIZE;

    struct page *base_page = &MEM_MAP[0];
    base_page->flags = PG_free;
    base_page->order = MAX_ORDER - 1;
    base_page->next = NULL;

    buddy_free_area[MAX_ORDER - 1] = base_page;

    printf("[System] Buddy Init: Managed %lu pages (%d MB)\n", total_pages, MEM_SIZE/1024/1024);
}

struct page *alloc_pages(int order) {
    int cur_order = order;

    while (cur_order < MAX_ORDER) {
        if (buddy_free_area[cur_order]) {
            struct page *page = buddy_free_area[cur_order];
            buddy_free_area[cur_order] = page->next;

            while (cur_order > order) {
                cur_order--;
                unsigned long pfn = page - MEM_MAP;
                unsigned long buddy_pfn = pfn + (1UL << cur_order);
                struct page *buddy = &MEM_MAP[buddy_pfn];

                buddy->flags = PG_free;
                buddy->order = cur_order;

                buddy->next = buddy_free_area[cur_order];
                buddy_free_area[cur_order] = buddy;
            }

            page->flags = PG_buddy;
            page->order = order;
            page->next = NULL; // 清空链表指针防止野指针
            return page;
        }
        cur_order++;
    }
    return NULL;
}

void __free_pages(struct page *page, int order) {
    unsigned long pfn = page - MEM_MAP;

    while (order < MAX_ORDER - 1) {
        unsigned long buddy_pfn = pfn ^ (1UL << order);
        struct page *buddy = &MEM_MAP[buddy_pfn];

        if (!(buddy->flags & PG_free) || buddy->order != order) {
            break;
        }

        // 从链表移除 buddy
        struct page **prev = &buddy_free_area[order];
        while (*prev && *prev != buddy) prev = &(*prev)->next;
        if (*prev) *prev = buddy->next;

        // 合并
        unsigned long combined_pfn = pfn & buddy_pfn;
        page = &MEM_MAP[combined_pfn];
        pfn = combined_pfn;
        order++;
    }

    page->flags = PG_free;
    page->order = order;
    page->next = buddy_free_area[order];
    buddy_free_area[order] = page;
}

// ================= 5. Slab Allocator =================

void slab_init() {
    for (int i = 0; i < SLAB_INDEX_COUNT; i++) {
        slab_caches[i].obj_size = slab_sizes[i];
        slab_caches[i].partial = NULL;
    }
    printf("[System] Slab Init.\n");
}

int cache_grow(kmem_cache_t *cache) {
    // 从buddy找一个最小页
    struct page *page = alloc_pages(0);
    if (!page) return 0;

    page->flags = PG_slab;
    page->slab_cache = cache;
    page->active_objects = 0;
    // 用页帧计算出真实的内存偏移（PA）
    void *addr = page_address(page);
    void **prev_obj_ptr = NULL;

    int count = PAGE_SIZE / cache->obj_size;

    // 构建 freelist (In-band metadata)
    for (int i = count - 1; i >= 0; i--) {
        void *obj = (uint8_t *)addr + (i * cache->obj_size);
        *(void **)obj = prev_obj_ptr;
        prev_obj_ptr = obj;
    }

    page->freelist = prev_obj_ptr;

    // 头插法 newPage->old_page
    page->next = cache->partial;
    cache->partial = page;

    return 1;
}

void *kmem_cache_alloc(kmem_cache_t *cache) {
    // 看当前内存池里面有没有
    if (!cache->partial) {
        // 没有的话分配
        if (!cache_grow(cache)) return NULL;
    }

    struct page *page = cache->partial;

    // 简单的空指针检查
    if (!page->freelist) {
        printf("Error: Page inside partial list has NULL freelist!\n");
        return NULL;
    }
    // obj是要取的PA，更新freelist，指向原来放在obj里面的PA
    void *obj = page->freelist;
    page->freelist = *(void **)obj;
    page->active_objects++;

    if (!page->freelist) {
        cache->partial = page->next;
        page->next = NULL;
    }

    memset(obj, 0, 8); // 清除内部链表数据
    return obj;
}

void kmem_cache_free(void *obj) {
    struct page *page = virt_to_page(obj);
    if (!page || !(page->flags & PG_slab)) return;

    kmem_cache_t *cache = page->slab_cache;

    // 把释放的位置写上freelist（原来下一个空闲的位置）
    *(void **)obj = page->freelist;
    page->freelist = obj;
    page->active_objects--;

    // 本来如果满了该page会变成游离状态（节省CPU资源）
    // 如果没满可以放回来
    int max_objs = PAGE_SIZE / cache->obj_size;
    if (page->active_objects == max_objs - 1) {
        page->next = cache->partial;
        cache->partial = page;
    }

    // 如果本页无活跃还给buddy
    if (page->active_objects == 0) {
        if (cache->partial == page) {
            cache->partial = page->next;
        } else {
             struct page **prev = &cache->partial;
             while (*prev && *prev != page) prev = &(*prev)->next;
             if (*prev) *prev = page->next;
        }

        page->flags = PG_buddy;
        __free_pages(page, 0);
    }
}

// ================= 6. Wrapper & Main =================

void kmalloc_init() {
    // [FIX] 增加错误检查
    PHYS_MEM_START = malloc(MEM_SIZE);
    if (!PHYS_MEM_START) {
        fprintf(stderr, "FATAL: Failed to allocate physical memory pool.\n");
        exit(1);
    }

    unsigned long num_pages = MEM_SIZE / PAGE_SIZE;
    MEM_MAP = (struct page *)malloc(num_pages * sizeof(struct page));
    if (!MEM_MAP) {
        fprintf(stderr, "FATAL: Failed to allocate mem_map.\n");
        exit(1);
    }

    memset(MEM_MAP, 0, num_pages * sizeof(struct page));

    buddy_init();
    slab_init();
}

void *kmalloc(size_t size) {
    // slub
    for (int i = 0; i < SLAB_INDEX_COUNT; i++) {
        if (size <= slab_sizes[i]) {
            // 寻找现在的内存池里面有没有空位
            return kmem_cache_alloc(&slab_caches[i]);
        }
    }

    // Buddy
    int order = 0;
    while ((PAGE_SIZE << order) < size) order++;

    struct page *page = alloc_pages(order);
    if (!page) return NULL;

    return page_address(page);
}

void kfree(void *ptr) {
    if (!ptr) return;

    struct page *page = virt_to_page(ptr);
    if (!page) {
        printf("Error: Invalid address %p\n", ptr);
        return;
    }

    if (page->flags & PG_slab) {
        kmem_cache_free(ptr);
    } else if (page->flags & PG_buddy) {
        __free_pages(page, page->order);
    } else {
        printf("Error: Double free or invalid page state %p (Flags: %x)\n", ptr, page->flags);
    }
}

int main() {
    kmalloc_init();
    printf("\n--- Test kmalloc (Linux Style: struct page & vmemmap) ---\n");

    // Slab 分配
    void *small = kmalloc(10);
    printf("Small alloc (10B): %p\n", small);

    void *mid = kmalloc(200);
    printf("Mid alloc (200B): %p\n", mid);

    // Buddy 分配
    void *large = kmalloc(10 * 1024 * 1024);
    if (large)
        printf("Large alloc (10MB): %p\n", large);
    else
        printf("Large alloc (10MB): Failed\n");

    // 验证 Metadata
    if (small) {
        struct page *p_small = virt_to_page(small);
        printf("[Debug] Small ptr page PFN: %ld, Flag: Slab\n", p_small - MEM_MAP);
    }

    if (large) {
        struct page *p_large = virt_to_page(large);
        printf("[Debug] Large ptr page PFN: %ld, Flag: Buddy, Order: %d\n",
            p_large - MEM_MAP, p_large->order);
    }

    printf("\n--- Freeing ---\n");
    kfree(mid);
    kfree(large);
    kfree(small);

    printf("Done.\n");
    return 0;
}