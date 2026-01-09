#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

// =================配置区域=================
#define MEM_SIZE        (128 * 1024 * 1024) // 128MB
#define PAGE_SIZE       (2 * 1024 * 1024)   // 2MB (Huge Page)
#define PAGE_COUNT      (MEM_SIZE / PAGE_SIZE) // 刚好 64 个页

// 模拟的物理内存基地址 (GPU VRAM Base)
static uint8_t *g_phys_base = NULL;

// 核心：只需一个 64 位整数即可管理 128MB
// 0 = 空闲, 1 = 占用
// Bit 0 对应 Page 0, Bit 63 对应 Page 63
static uint64_t g_bitmap = 0;

// =================核心逻辑=================

void bitmap_init() {
    // 模拟申请物理内存
    g_phys_base = (uint8_t *)malloc(MEM_SIZE);
    if (!g_phys_base) {
        printf("Fatal: OOM\n");
        exit(1);
    }

    // 初始化位图，0 表示全空
    g_bitmap = 0;

    printf("[System] Init: 128MB VRAM, 2MB Page, Total 64 Pages.\n");
    printf("[System] Bitmap Manager Size: 8 Bytes (1x uint64_t)\n");
}

/**
 * 分配 n 个连续的 2MB 页
 * @param num_pages 需要分配的页数
 * @return void* 指向第一页的指针，失败返回 NULL
 */
void *bitmap_alloc(int num_pages) {
    if (num_pages <= 0 || num_pages > PAGE_COUNT) return NULL;

    // 1. 生成掩码 (Mask)
    // 如果要分配 3 页，mask = binary 111 (0x7)
    // 注意处理 64 的边界情况
    uint64_t mask = (num_pages == 64) ? ~0ULL : ((1ULL << num_pages) - 1);

    // 2. 滑动窗口搜索 (Scanning)
    // 这是一个简单的 O(N) 搜索，但因为 N 最大只有 64，且是纯寄存器操作，极快。
    for (int i = 0; i <= PAGE_COUNT - num_pages; i++) {
        // 检查 g_bitmap 从第 i 位开始的 num_pages 位是否全是 0
        // (g_bitmap >> i) 把第 i 位移到最低位
        // & mask 取出最低的 num_pages 位
        if (((g_bitmap >> i) & mask) == 0) {

            // 找到空闲区域！

            // 3. 标记为占用 (Set bits)
            // 把 mask 移回第 i 位，然后 OR 上去
            g_bitmap |= (mask << i);

            printf("[Alloc] Found %d pages at Index %d\n", num_pages, i);

            // 4. 返回物理地址
            return (void *)(g_phys_base + (uint64_t)i * PAGE_SIZE);
        }
    }

    printf("[Alloc] Failed to find %d contiguous pages.\n", num_pages);
    return NULL;
}

/**
 * 释放内存
 * @param ptr 分配时返回的指针
 * @param num_pages 必须记住当时分配了多少页 (通常由上层记录)
 */
void bitmap_free(void *ptr, int num_pages) {
    if (!ptr || num_pages <= 0) return;

    // 1. 计算页索引
    uint64_t offset = (uint8_t *)ptr - g_phys_base;
    int index = offset / PAGE_SIZE;

    // 边界检查
    if (index < 0 || index + num_pages > PAGE_COUNT) {
        printf("[Free] Error: Invalid pointer or size.\n");
        return;
    }

    // 2. 生成掩码
    uint64_t mask = (num_pages == 64) ? ~0ULL : ((1ULL << num_pages) - 1);

    // 3. 标记为空闲 (Clear bits)
    // 将掩码移到位置，取反，然后 AND
    // 例如：Bitmap = ...11100... (中间三个是1)
    // Mask << i  = ...11100...
    // ~(Mask<<i) = ...00011...
    // AND 操作后 = ...00000...
    g_bitmap &= ~(mask << index);

    printf("[Free] Freed %d pages at Index %d. Bitmap: 0x%016lx\n", num_pages, index, g_bitmap);
}

// 调试工具：打印位图状态
void bitmap_dump() {
    printf("Map: ");
    for (int i = 0; i < 64; i++) {
        printf("%lu", (g_bitmap >> i) & 1);
    }
    printf("\n");
}

// =================测试主函数=================

int main() {
    bitmap_init();

    // 1. 分配单个页 (最常见场景)
    void *p1 = bitmap_alloc(1); // Index 0
    void *p2 = bitmap_alloc(2); // Index 1, 2 (需要连续)
    void *p3 = bitmap_alloc(1); // Index 3

    bitmap_dump();

    // 2. 释放中间的 p2
    printf("\n--- Freeing p2 (2 pages) ---\n");
    bitmap_free(p2, 2);
    bitmap_dump();
    // 现在的状态应该是 1 0 0 1 ... (Index 0占, 1-2空, 3占)

    // 3. 尝试分配 2 页 (应该填补中间的空洞)
    printf("\n--- Allocating 2 pages (Should fill the hole) ---\n");
    void *p4 = bitmap_alloc(2);
    // p4 应该等于 p2 的地址
    if (p4 == p2) {
        printf("Success: Hole filled correctly at %p\n", p4);
    } else {
        printf("Note: Allocated at %p (Not best fit, but first fit)\n", p4);
    }
    bitmap_dump();

    // 4. 尝试分配超大内存
    printf("\n--- Allocating 60 pages ---\n");
    void *p5 = bitmap_alloc(60); // 剩余 64-4 = 60页，应该刚好成功
    if (p5) printf("Big alloc success.\n");
    else printf("Big alloc failed.\n");
    bitmap_free(p4, 2);
    bitmap_free(p1, 1);
    bitmap_free(p3, 1);
    bitmap_free(p5, 60);
    bitmap_dump();
    return 0;
}