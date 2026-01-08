#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

// ================= 1. 内核基础设施模拟 =================

#define PAGE_SIZE 4096
#define MEM_SIZE (16 * 1024 * 1024) // 模拟 16MB 物理内存

// 模拟物理内存基地址
uint8_t *g_phys_mem_base;

// 模拟 Linux 的 struct page
// 真实内核中，struct page 是一个巨大的联合体(union)
// 当它被 SLUB 使用时，字段被复用为以下样子：
typedef struct page {
    union {
        // 当页空闲给 Buddy 时用的字段
        struct {
             struct page *next_buddy;
             int order;
        };
        // 当页被 SLUB 接管后用的字段 (Slab Meta)
        struct {
            void *freelist;      // 指向页内第一个空闲对象
            short inuse;         // 已分配对象计数
            short objects;       // 总对象数
            struct page *next;   // Partial 链表指针
            struct kmem_cache *slab_cache; // 指回它所属的 cache
        };
    };
} struct_page;

struct_page *g_mem_map; // 全局页描述符数组

// 辅助：虚拟地址转页描述符 (virt_to_page)
struct_page *virt_to_page(void *addr) {
    unsigned long offset = (uint8_t *)addr - g_phys_mem_base;
    unsigned long pfn = offset / PAGE_SIZE;
    return &g_mem_map[pfn];
}

// 模拟 Buddy：简单分配一页
void *alloc_pages(int order) {
    // 这里偷懒直接从堆顶切一页，模拟物理页分配
    static int allocated_pages = 0;
    void *addr = g_phys_mem_base + (allocated_pages * PAGE_SIZE);
    allocated_pages++;
    return addr;
}

// ================= 2. SLUB 核心定义 =================

// 定义 kmem_cache (比如 kmalloc-64 就是一个这样的结构体)
typedef struct kmem_cache {
    const char *name;
    int size;               // 对象大小 (如 64)
    int offset;             // Free pointer 的偏移量 (通常是 0)
    struct page *cpu_slab;  // 【核心】当前 CPU 正在使用的活跃 Slab
    struct page *partial;   // 部分空闲的 Slab 链表
} kmem_cache;

// ================= 3. SLUB 核心逻辑 =================

// 初始化一个新的 Slab (从 Buddy 拿一页，建立 freelist)
static void setup_slab(kmem_cache *s, struct page *page) {
    void *start = g_phys_mem_base + ((page - g_mem_map) * PAGE_SIZE);

    page->objects = PAGE_SIZE / s->size;
    page->inuse = 0;
    page->slab_cache = s;

    // 【核心黑科技】构建对象内的单向链表
    // 每一个空闲对象的前 8 字节，存储下一个对象的地址
    void *p = start;
    for (int i = 0; i < page->objects - 1; i++) {
        void *next = (char *)p + s->size;
        *(void **)p = next; // 当前对象的内容 = 下一个对象的地址
        p = next;
    }
    *(void **)p = NULL; // 最后一个指向 NULL

    page->freelist = start; // 页描述符指向第一个对象

    printf("[SLUB Debug] New Slab for %s: Page PFN %ld, Objs: %d\n",
           s->name, page - g_mem_map, page->objects);
}

// 分配对象
void *kmem_cache_alloc(kmem_cache *s) {
    struct page *page = s->cpu_slab;

    // 1. Fast Path: 当前活跃 Slab 还有空间
    // 真实内核汇编里，这段非常短，甚至无锁
    if (page && page->freelist) {
        void *object = page->freelist;

        // 【核心操作】读取对象内部的指针，更新 freelist
        void *next_object = *(void **)object;
        page->freelist = next_object;

        page->inuse++;
        return object;
    }

    // 2. Slow Path: 活跃 Slab 满了或为空
    // 尝试从 partial 链表拿，或者找 Buddy 要新页
    // (为了代码简洁，这里直接要新页，并设为 cpu_slab)
    void *new_phys = alloc_pages(0);
    struct page *new_page = virt_to_page(new_phys);
    setup_slab(s, new_page);

    s->cpu_slab = new_page;

    // 递归调用一次 Fast Path
    return kmem_cache_alloc(s);
}

// 释放对象
void kmem_cache_free(void *obj) {
    // 1. 通过地址反查 struct page (virt_to_page)
    struct page *page = virt_to_page(obj);
    kmem_cache *s = page->slab_cache;

    // 2. 头插法放回 freelist
    // 让 obj 指向原本的 head
    *(void **)obj = page->freelist;
    // 让 head 指向 obj
    page->freelist = obj;

    page->inuse--;

    printf("[Free] Obj %p returned to %s (Inuse: %d)\n",
           obj, s->name, page->inuse);
}

// ================= 4. 模拟 kmalloc 体系 =================

// 内核里有一组预定义的 caches
#define KMALLOC_SHIFT_LOW 3
#define KMALLOC_SHIFT_HIGH 10 // 支持到 1024 字节
kmem_cache kmalloc_caches[KMALLOC_SHIFT_HIGH + 1];

// 初始化 kmalloc-8, kmalloc-16, kmalloc-32 ... kmalloc-1024
void kmem_cache_init() {
    // 申请大块内存作为物理内存
    g_phys_mem_base = malloc(MEM_SIZE);
    // 申请页描述符数组
    g_mem_map = malloc((MEM_SIZE / PAGE_SIZE) * sizeof(struct_page));
    memset(g_mem_map, 0, (MEM_SIZE / PAGE_SIZE) * sizeof(struct_page));

    // 创建通用缓存
    for (int i = 3; i <= KMALLOC_SHIFT_HIGH; i++) {
        int size = 1 << i; // 8, 16, 32, 64...
        kmalloc_caches[i].size = size;
        kmalloc_caches[i].offset = 0;

        // 名字 trick (简单处理)
        kmalloc_caches[i].name = malloc(32);
        sprintf((char*)kmalloc_caches[i].name, "kmalloc-%d", size);
    }
    printf("SLUB initialized. RAM Base: %p\n", g_phys_mem_base);
}

// 真正的 kmalloc 接口
void *kmalloc(size_t size) {
    // 1. 找到合适的桶 (Index)
    // 简单算法：找到比 size 大的最小的 2^n
    int index = 0;
    if (size <= 8) index = 3;
    else if (size <= 16) index = 4;
    else if (size <= 32) index = 5;
    else if (size <= 64) index = 6;
    else if (size <= 128) index = 7;
    // ... 省略更大
    else index = 10; // 默认最大演示到 1024

    // 2. 委托给对应的 SLUB Cache
    printf("[kmalloc] Request %zu bytes -> using %s\n", size, kmalloc_caches[index].name);
    return kmem_cache_alloc(&kmalloc_caches[index]);
}

void kfree(void *obj) {
    kmem_cache_free(obj);
}

// ================= 5. 测试主程序 =================

int main() {
    kmem_cache_init();
    printf("----------------------------------------\n");

    // 场景 1：申请 50 字节
    // 预期：命中 kmalloc-64
    void *p1 = kmalloc(50);
    printf("Got pointer p1: %p\n", p1);

    // 场景 2：申请 20 字节
    // 预期：命中 kmalloc-32
    void *p2 = kmalloc(20);
    printf("Got pointer p2: %p\n", p2);

    // 场景 3：连续申请，观察对象复用
    void *p3 = kmalloc(50); // kmalloc-64
    printf("Got pointer p3: %p\n", p3);

    // 验证：看看 p1 里面的内容
    // 此时 p1 是分配出去的，里面是垃圾数据。
    // 我们写入数据模拟使用
    strcpy((char*)p1, "User Data");

    printf("\n--- Freeing p1 ---\n");
    kfree(p1);
    // 此时 p1 被释放回 kmalloc-64 的 Slab。
    // 【关键验证】：p1 的前8字节现在应该变成了 freelist 的头指针！
    // 让我们偷看一下 p1 内存里的值（虽然这在用户态是不安全的，但在模拟器里可以看）

    printf("\n--- Allocating p4 (Same size as p1) ---\n");
    // 因为 SLUB 是 LIFO (栈式)，刚刚释放的 p1 应该马上被 p4 拿到
    void *p4 = kmalloc(50);
    printf("Got pointer p4: %p (Should equal p1: %p)\n", p4, p1);

    return 0;
}