#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

// =================配置参数=================
#define HEAP_SIZE (128 * 1024 * 1024) // 总堆大小 128MB
#define MIN_PAGE_SIZE (2 * 1024 * 1024) // 最小粒度 2MB (Order 0)

// 计算最大阶数：128MB / 2MB = 64个块 = 2^6
// Order 0: 2MB, Order 1: 4MB ... Order 6: 128MB
#define MAX_ORDER 6 

// =================数据结构=================

// 链表节点（直接嵌入在空闲内存中，或者使用元数据管理）
// 这里为了代码清晰，使用独立的链表结构
typedef struct FreeNode {
    int page_idx; // 对应物理内存的第几个 2MB 页
    struct FreeNode *prev;
    struct FreeNode *next;
} FreeNode;

// 页描述符（Metadata），用于记录每一页的状态
// 在真实系统中，这通常是个位图或压缩结构，这里用数组模拟
typedef struct {
    bool is_free;
    int order; // 如果被分配或作为空闲块头，记录当前块的阶数
} PageDescriptor;

// 全局状态
uint8_t *g_heap_base = NULL;          // 模拟物理内存基地址
PageDescriptor *g_page_desc = NULL;   // 页描述符数组
FreeNode *g_free_area[MAX_ORDER + 1]; // 空闲链表数组 (Order 0 ~ 6)
int g_total_pages = 0;

// =================辅助函数=================

// 初始化堆
void buddy_init() {
    g_heap_base = (uint8_t *)malloc(HEAP_SIZE);
    g_total_pages = HEAP_SIZE / MIN_PAGE_SIZE; // 64个页面
    
    // 初始化页描述符
    g_page_desc = (PageDescriptor *)malloc(sizeof(PageDescriptor) * g_total_pages);
    for (int i = 0; i < g_total_pages; i++) {
        g_page_desc[i].is_free = false; // 初始全是无效的，稍后合并出一个大的
        g_page_desc[i].order = 0;
    }

    // 初始化空闲链表
    for (int i = 0; i <= MAX_ORDER; i++) {
        g_free_area[i] = NULL;
    }

    // 初始状态：整个堆是一个巨大的 Max Order 块
    // 我们把第0个页（代表整个128MB）挂入 Order 6
    FreeNode *root = (FreeNode *)malloc(sizeof(FreeNode));
    root->page_idx = 0;
    root->next = root->prev = NULL;
    
    g_free_area[MAX_ORDER] = root;
    g_page_desc[0].is_free = true;
    g_page_desc[0].order = MAX_ORDER;

    printf("[Init] Heap initialized. Base: %p, Total 2MB Pages: %d\n", g_heap_base, g_total_pages);
}

// 链表操作：插入头部
void list_add(int order, FreeNode *node) {
    node->next = g_free_area[order];
    node->prev = NULL;
    if (g_free_area[order]) {
        g_free_area[order]->prev = node;
    }
    g_free_area[order] = node;
}

// 链表操作：移除节点
void list_remove(int order, FreeNode *node) {
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        g_free_area[order] = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    }
}

// 计算需要的阶数
int get_needed_order(size_t size) {
    if (size <= MIN_PAGE_SIZE) return 0;
    
    // 计算 size 需要多少个 2MB 页
    size_t num_pages = (size + MIN_PAGE_SIZE - 1) / MIN_PAGE_SIZE;
    
    // 找到满足 2^order >= num_pages 的最小 order
    int order = 0;
    while ((1 << order) < num_pages) {
        order++;
    }
    return order;
}

// =================核心：内存申请=================

void *buddy_alloc(size_t size) {
    int target_order = get_needed_order(size);
    if (target_order > MAX_ORDER) return NULL; // 请求太大

    printf("[Alloc] Request: %zu bytes -> Order %d\n", size, target_order);

    // 1. 向上查找可用的空闲块
    int current_order = target_order;
    while (current_order <= MAX_ORDER && g_free_area[current_order] == NULL) {
        current_order++;
    }

    // OOM (没有足够大的块)
    if (current_order > MAX_ORDER) return NULL;

    // 2. 摘下一个块
    FreeNode *block = g_free_area[current_order];
    list_remove(current_order, block);

    // 3. 如果块太大，进行分裂 (Split) 直到满足 target_order
    while (current_order > target_order) {
        current_order--; // 降级
        
        int buddy_idx = block->page_idx + (1 << current_order); // 计算右半部分(Buddy)的索引
        
        printf("  >> Splitting Order %d at idx %d -> Left: %d, Right(Buddy): %d\n", 
               current_order + 1, block->page_idx, block->page_idx, buddy_idx);

        // 创建 Buddy 节点并挂入低一级的链表
        FreeNode *buddy = (FreeNode *)malloc(sizeof(FreeNode));
        buddy->page_idx = buddy_idx;
        
        // 更新 metadata
        g_page_desc[buddy_idx].is_free = true;
        g_page_desc[buddy_idx].order = current_order;

        list_add(current_order, buddy);
    }

    // 4. 分配完成，标记状态
    g_page_desc[block->page_idx].is_free = false;
    g_page_desc[block->page_idx].order = target_order; // 记录分配时的阶数，free时要用
    
    void *addr = g_heap_base + (block->page_idx * MIN_PAGE_SIZE);
    free(block); // 释放链表节点容器
    return addr;
}

// =================核心：内存释放=================

void buddy_free(void *ptr) {
    if (!ptr) return;

    // 1. 计算页索引
    int page_idx = ((uint8_t *)ptr - g_heap_base) / MIN_PAGE_SIZE;
    int order = g_page_desc[page_idx].order; // 获取该块的大小

    printf("[Free] Ptr %p (Idx %d), Order %d\n", ptr, page_idx, order);

    // 循环尝试合并
    while (order < MAX_ORDER) {
        // 核心魔法：计算 Buddy 索引
        // 异或运算直接翻转二进制中对应 Order 的位
        int buddy_idx = page_idx ^ (1 << order); 

        // 检查范围
        if (buddy_idx >= g_total_pages) break;

        // 检查 Buddy 是否能合并：
        // 1. 必须是空闲的
        // 2. 必须是完整的 (Order 必须相同，防止合并了分裂块的一部分)
        if (!g_page_desc[buddy_idx].is_free || g_page_desc[buddy_idx].order != order) {
            printf("  >> Cannot merge with buddy %d (Free:%d, Order:%d)\n", 
                   buddy_idx, g_page_desc[buddy_idx].is_free, g_page_desc[buddy_idx].order);
            break;
        }

        printf("  >> Merging idx %d with buddy %d -> Order %d\n", page_idx, buddy_idx, order + 1);

        // 2. 从链表中移除 Buddy
        // 注意：这里需要遍历链表找到 buddy_idx 对应的节点 (真实实现通常用双向链表+节点嵌入直接定位)
        FreeNode *curr = g_free_area[order];
        while (curr) {
            if (curr->page_idx == buddy_idx) {
                list_remove(order, curr);
                free(curr);
                break;
            }
            curr = curr->next;
        }

        // 3. 准备下一轮合并
        // 合并后的新块索引是两者中较小的那个
        if (buddy_idx < page_idx) {
            page_idx = buddy_idx; 
        }
        
        // 升级
        order++;
    }

    // 4. 将最终合并好的块挂入对应 Order 的链表
    FreeNode *node = (FreeNode *)malloc(sizeof(FreeNode));
    node->page_idx = page_idx;
    
    g_page_desc[page_idx].is_free = true;
    g_page_desc[page_idx].order = order;
    
    list_add(order, node);
    printf("  >> Block placed in Order %d list\n", order);
}

// =================测试主程序=================

int main() {
    buddy_init();
    printf("--------------------------------\n");

    // 申请 3MB -> 需要 4MB (Order 1)
    // 过程：Order 6 分裂 -> Order 5 ... -> Order 1
    void *p1 = buddy_alloc(3 * 1024 * 1024); 
    printf("P1 allocated at: %p\n", p1);
    printf("--------------------------------\n");

    // 申请 7MB -> 需要 8MB (Order 2)
    void *p2 = buddy_alloc(7 * 1024 * 1024);
    printf("P2 allocated at: %p\n", p2);
    printf("--------------------------------\n");

    // 释放 P1 (Order 1)
    buddy_free(p1);
    printf("--------------------------------\n");

    // 释放 P2 (Order 2)
    // 这里不会立即发生大合并，因为 P1 虽然还了，但可能需要 P1 的伙伴也空闲才能合成 Order 2
    buddy_free(p2);
    
    return 0;
}
