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

typedef struct FreeNode {
    int page_idx; // 对应物理内存的第几个 2MB 页
    struct FreeNode *prev;
    struct FreeNode *next;
} FreeNode;

typedef struct {
    bool is_free;
    int order; // 如果被分配或作为空闲块头，记录当前块的阶数
} PageDescriptor;

// 全局状态
uint8_t *g_heap_base = NULL;          // 模拟物理内存基地址
PageDescriptor *g_page_desc = NULL;   // 页描述符数组
FreeNode *g_free_area[MAX_ORDER + 1]; // 空闲链表数组 (Order 0 ~ 6)
int g_total_pages = 0;

// ================= 辅助打印函数 =================
// 打印当前堆的空闲链表状态
void debug_print_heap_status() {
    printf("\n[DEBUG] === Current Heap Status ===\n");
    for (int i = MAX_ORDER; i >= 0; i--) {
        printf("  Order %d (%3dMB): ", i, (1 << i) * 2);
        FreeNode *curr = g_free_area[i];
        if (!curr) {
            printf("(empty)");
        }
        int count = 0;
        while (curr) {
            printf("[%d] -> ", curr->page_idx);
            curr = curr->next;
            count++;
        }
        if (count > 0) printf("NULL");
        printf("\n");
    }
    printf("===================================\n\n");
}

// =================辅助函数=================
void buddy_init() {

    g_heap_base = (uint8_t *)malloc(HEAP_SIZE);
    g_total_pages = HEAP_SIZE / MIN_PAGE_SIZE;

    g_page_desc = (PageDescriptor *)malloc(sizeof(PageDescriptor) * g_total_pages);
    for (int i = 0; i < g_total_pages; i++) {
        g_page_desc[i].is_free = false;
        g_page_desc[i].order = 0;
    }

    for (int i = 0; i <= MAX_ORDER; i++) {
        g_free_area[i] = NULL;
    }

    // 初始状态：整个堆挂入 Max Order
    FreeNode *root = (FreeNode *)malloc(sizeof(FreeNode));
    root->page_idx = 0;
    root->next = root->prev = NULL;

    g_free_area[MAX_ORDER] = root;
    g_page_desc[0].is_free = true;
    g_page_desc[0].order = MAX_ORDER;

    printf("[Init] Heap initialized. Base: %p, Total Pages: %d, Max Order: %d\n", g_heap_base, g_total_pages, MAX_ORDER);
    debug_print_heap_status();
}

void list_add(int order, FreeNode *node) {
    node->next = g_free_area[order];
    node->prev = NULL;
    if (g_free_area[order]) {
        g_free_area[order]->prev = node;
    }
    g_free_area[order] = node;
}

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

int get_needed_order(size_t size) {
    if (size <= MIN_PAGE_SIZE) return 0;
    size_t num_pages = (size + MIN_PAGE_SIZE - 1) / MIN_PAGE_SIZE;
    int order = 0;
    while ((1 << order) < num_pages) {
        order++;
    }
    return order;
}

// =================核心：内存申请=================

void *buddy_alloc(size_t size) {
    // 在这里获取想要order的大小
    int target_order = get_needed_order(size);
    if (target_order > MAX_ORDER) {
        printf("[Alloc] Failed: Size %zu too large (>%dMB)\n", size, (1<<MAX_ORDER)*2);
        return NULL;
    }

    printf("[Alloc] Request: %zu bytes (Need Order %d, %dMB)\n", size, target_order, (1<<target_order)*2);

    // 1. 向上查找可用的空闲块
    int current_order = target_order;
    // 小于最大order && 当前无可分配节点
    while (current_order <= MAX_ORDER && g_free_area[current_order] == NULL) {
        current_order++;
    }

    if (current_order > MAX_ORDER) {
        printf("[Alloc] Failed: OOM (Out Of Memory)\n");
        return NULL;
    }

    printf("  >> Found free block at Order %d\n", current_order);

    // 2. 摘下一个块
    FreeNode *block = g_free_area[current_order];
    list_remove(current_order, block);

    // 3. 分裂循环
    while (current_order > target_order) {
        current_order--; // 降级

        int buddy_idx = block->page_idx + (1 << current_order);

        printf("  >> Splitting Order %d [Idx %d] into Order %d:\n",
               current_order + 1, block->page_idx, current_order);
        printf("     |-- Left  (Idx %d): Keep for alloc\n", block->page_idx);
        printf("     |-- Right (Idx %d): Buddy, return to free list\n", buddy_idx);

        FreeNode *buddy = (FreeNode *)malloc(sizeof(FreeNode));
        buddy->page_idx = buddy_idx;

        g_page_desc[buddy_idx].is_free = true;
        g_page_desc[buddy_idx].order = current_order;

        list_add(current_order, buddy);
    }

    // 4. 分配完成
    g_page_desc[block->page_idx].is_free = false;
    g_page_desc[block->page_idx].order = target_order;

    void *addr = g_heap_base + (block->page_idx * MIN_PAGE_SIZE);
    printf("[Alloc] Success! Addr: %p (Idx %d)\n", addr, block->page_idx);

    free(block);
    debug_print_heap_status(); // 分配完打印状态
    return addr;
}

// =================核心：内存释放=================

void buddy_free(void *ptr) {
    if (!ptr) return;

    int page_idx = ((uint8_t *)ptr - g_heap_base) / MIN_PAGE_SIZE;
    int order = g_page_desc[page_idx].order;

    printf("[Free] Ptr %p (Idx %d), Order %d (%dMB)\n", ptr, page_idx, order, (1<<order)*2);

    // 循环尝试合并
    while (order < MAX_ORDER) {
        int buddy_idx = page_idx ^ (1 << order);

        // 越界检查
        if (buddy_idx >= g_total_pages) {
            printf("  >> Stop: Buddy idx %d out of range\n", buddy_idx);
            break;
        }

        // 获取 Buddy 状态信息用于打印
        bool buddy_free = g_page_desc[buddy_idx].is_free;
        int buddy_order = g_page_desc[buddy_idx].order;

        printf("  >> Checking buddy Idx %d (Order %d): ", buddy_idx, order);

        // 合并条件检查
        if (!buddy_free || buddy_order != order) {
            if (!buddy_free) printf("Busy (Cannot merge)\n");
            else printf("Free but Order mismatch (Is %d, Need %d)\n", buddy_order, order);
            break;
        }

        printf("Match! Merging...\n");

        // 从链表中移除 Buddy
        FreeNode *curr = g_free_area[order];
        FreeNode *prev = NULL;
        bool found = false;

        // 手动遍历查找并移除（模拟真实操作）
        while (curr) {
            if (curr->page_idx == buddy_idx) {
                if (prev) prev->next = curr->next;
                else g_free_area[order] = curr->next;
                if (curr->next) curr->next->prev = prev;
                free(curr);
                found = true;
                break;
            }
            prev = curr;
            curr = curr->next;
        }

        if (!found) {
             printf("  !! Error: Buddy marked free but not found in list!\n");
             break;
        }

        // 更新索引为两者中较小的那个 (左边的)
        int old_idx = page_idx;
        if (buddy_idx < page_idx) {
            page_idx = buddy_idx;
        }

        printf("     Merged Idx %d + Idx %d -> New Block Idx %d (Order %d)\n",
               old_idx, buddy_idx, page_idx, order + 1);

        order++;
    }

    // 挂入链表
    FreeNode *node = (FreeNode *)malloc(sizeof(FreeNode));
    node->page_idx = page_idx;

    g_page_desc[page_idx].is_free = true;
    g_page_desc[page_idx].order = order;

    list_add(order, node);
    printf("  >> Block Idx %d placed in Order %d list\n", page_idx, order);
    debug_print_heap_status(); // 释放完打印状态
}

// =================测试主程序=================

int main() {
    buddy_init();

    void *p1 = buddy_alloc(1 * 1024 * 1024);
    void *p2 = buddy_alloc(1 * 1024 * 1024);
    void *p3 = buddy_alloc(1 * 1024 * 1024);
    void *p4 = buddy_alloc(1 * 1024 * 1024);
    void *p5 = buddy_alloc(1 * 1024 * 1024);


    buddy_free(p1);
    buddy_free(p2);
    buddy_free(p3);
    buddy_free(p4);
    buddy_free(p5);

    return 0;
}