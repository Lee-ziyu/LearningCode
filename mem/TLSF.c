#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

// ================= 配置参数 =================
#define MEM_SIZE        (128 * 1024 * 1024) // 128 MB
#define FL_INDEX_MAX    32                  // 支持最大 4GB
#define SL_INDEX_COUNT  4                   // 第二级分 4 份
#define SL_INDEX_SHIFT  2                   // log2(4) = 2

// 块状态标记
#define BLOCK_FREE      1
#define BLOCK_USED      0

// ================= 数据结构 =================

typedef struct block_header_t {
    // [物理邻居管理]
    struct block_header_t *phys_prev; // 物理上的左邻居 (刚才解释的"便利贴")
    size_t size;                      // 当前块大小 (包含头)
    int free_flag;                    // 1 = Free, 0 = Used

    // [逻辑链表管理] (只有是空闲块时才有效，Used块这里存用户数据)
    struct block_header_t *prev_free;
    struct block_header_t *next_free;
} block_header_t;

// TLSF 控制结构
typedef struct {
    block_header_t *block_null; // 哨兵
    uint32_t fl_bitmap;         // 第一级位图
    uint32_t sl_bitmap[FL_INDEX_MAX]; // 第二级位图组

    // 链表矩阵
    block_header_t *blocks[FL_INDEX_MAX][SL_INDEX_COUNT];
} tlsf_t;

// 全局实例
tlsf_t *tlsf_inst = NULL;
void *heap_start = NULL;

// ================= 辅助函数：位操作与索引计算 =================

// 查找最高位是第几位 (类似 log2)
static inline int tlsf_fls(size_t size) {
    if (size == 0) return -1;
    return 31 - __builtin_clz(size);
}

// 查找最低位是第几位 (用于在位图中找空闲位)
static inline int tlsf_ffs(uint32_t word) {
    if (word == 0) return -1;
    return __builtin_ctz(word);
}

// 根据大小计算 FL 和 SL 索引
void mapping_insert(size_t size, int *fl, int *sl) {
    *fl = tlsf_fls(size);
    // SL 计算逻辑：取 FL 下一级的若干位
    // 例如 size = 101100... (二进制)
    // FL 对应最高位 1，SL 对应紧接在后面的 SL_INDEX_SHIFT 位
    *sl = (size >> (*fl - SL_INDEX_SHIFT)) ^ (1 << SL_INDEX_SHIFT);
}

// 根据 FL 和 SL 查找对应的链表操作时需要的“最小尺寸” (Round Up)
void mapping_search(size_t size, int *fl, int *sl) {
    *fl = tlsf_fls(size);
    *sl = (size >> (*fl - SL_INDEX_SHIFT)) ^ (1 << SL_INDEX_SHIFT);

    // 这里的逻辑稍微复杂一点：如果 size 比当前 split 出来的格子稍微大一点点，
    // 我们必须去更大的格子里找，所以可能要进位。
    // 为了简化代码，这里假设简单的向下取整查找，配合后面的 round up。
}

// ================= 核心操作：链表管理 =================

void insert_free_block(block_header_t *block) {
    int fl, sl;
    mapping_insert(block->size, &fl, &sl);

    block->free_flag = BLOCK_FREE;

    // 头插法插入矩阵
    block->next_free = tlsf_inst->blocks[fl][sl];
    block->prev_free = NULL;

    if (block->next_free)
        block->next_free->prev_free = block;

    tlsf_inst->blocks[fl][sl] = block;

    // 更新位图
    tlsf_inst->fl_bitmap |= (1 << fl);
    tlsf_inst->sl_bitmap[fl] |= (1 << sl);
}

void remove_free_block(block_header_t *block) {
    int fl, sl;
    mapping_insert(block->size, &fl, &sl);

    // 从链表移除
    if (block->prev_free)
        block->prev_free->next_free = block->next_free;
    else
        tlsf_inst->blocks[fl][sl] = block->next_free;

    if (block->next_free)
        block->next_free->prev_free = block->prev_free;

    // 如果链表空了，更新位图
    if (tlsf_inst->blocks[fl][sl] == NULL) {
        tlsf_inst->sl_bitmap[fl] &= ~(1 << sl);
        if (tlsf_inst->sl_bitmap[fl] == 0) {
            tlsf_inst->fl_bitmap &= ~(1 << fl);
        }
    }
}

// ================= 核心操作：物理分割与合并 =================

// 切割块：block 是当前大块，size 是需要切分出去的大小
// 返回切分出去的块指针 (其实就是 block 自己，因为是头部切割)
block_header_t* block_split(block_header_t *block, size_t size) {
    size_t remaining_size = block->size - size;

    // 只有剩余空间足够放一个 Header 才有必要切
    if (remaining_size > sizeof(block_header_t)) {
        // 1. 算出剩余块的物理地址
        block_header_t *remaining = (block_header_t *)((char *)block + size);

        // 2. 初始化剩余块
        remaining->size = remaining_size;
        remaining->free_flag = BLOCK_FREE; // 暂时标记，之后会insert

        // ----------------------------------------------------
        // [关键] 刚才解释的 Step 2 & 3: 更新 phys_prev
        // ----------------------------------------------------
        // 剩余块的左邻居是当前切出去的 block
        remaining->phys_prev = block;

        // 如果剩余块右边还有块，需要通知右边的块：“你的左邻居换人了，换成 remaining 了”
        block_header_t *next_phys = (block_header_t *)((char *)remaining + remaining_size);
        // 简单的边界检查，假设内存末尾是安全的或者有哨兵
        if ((void*)next_phys < (void*)((char*)heap_start + MEM_SIZE)) {
            next_phys->phys_prev = remaining;
        }

        // 3. 调整当前块
        block->size = size;

        // 4. 把切剩下的部分放回空闲池
        insert_free_block(remaining);
    } else {
        // 剩余太小，不切了，全给用户
        size = block->size;
    }

    block->free_flag = BLOCK_USED;
    return block;
}

// 合并块：尝试向左向右合并
block_header_t* block_merge(block_header_t *block) {
    // 1. 尝试向右合并 (物理高地址)
    block_header_t *next_phys = (block_header_t *)((char *)block + block->size);
    // 检查越界
    if ((void*)next_phys < (void*)((char*)heap_start + MEM_SIZE)) {
        if (next_phys->free_flag == BLOCK_FREE) {
            // 右边是空的，吃掉它
            remove_free_block(next_phys);
            block->size += next_phys->size;

            // 更新更右边那个块的左邻居指针
            block_header_t *next_next_phys = (block_header_t *)((char *)block + block->size);
            if ((void*)next_next_phys < (void*)((char*)heap_start + MEM_SIZE)) {
                next_next_phys->phys_prev = block;
            }
        }
    }

    // 2. 尝试向左合并 (物理低地址) - [关键] 使用 phys_prev
    if (block->phys_prev && block->phys_prev->free_flag == BLOCK_FREE) {
        block_header_t *prev_phys = block->phys_prev;

        // 把左边那个从空闲表里拿出来
        remove_free_block(prev_phys);

        // 左边吃掉右边(当前block)
        prev_phys->size += block->size;

        // 更新右边邻居的 phys_prev
        block_header_t *next_neighbor = (block_header_t *)((char *)prev_phys + prev_phys->size);
        if ((void*)next_neighbor < (void*)((char*)heap_start + MEM_SIZE)) {
            next_neighbor->phys_prev = prev_phys;
        }

        block = prev_phys;
    }

    return block;
}

// ================= API 实现 =================

void tlsf_init(void *mem, size_t size) {
    heap_start = mem;
    tlsf_inst = (tlsf_t *)malloc(sizeof(tlsf_t));
    memset(tlsf_inst, 0, sizeof(tlsf_t));

    // 在内存起始处创建第一个大块
    block_header_t *first_block = (block_header_t *)mem;
    first_block->size = size;
    first_block->free_flag = BLOCK_FREE;
    first_block->phys_prev = NULL; // 最左边没有邻居

    insert_free_block(first_block);
    printf("[System] Init TLSF with %lu MB\n", size / 1024 / 1024);
}

void *tlsf_malloc(size_t size) {
    // 加上头部开销并对齐
    size_t adjust_size = size + sizeof(block_header_t);
    if (adjust_size < 32) adjust_size = 32;

    int fl, sl;
    mapping_insert(adjust_size, &fl, &sl);

    // O(1) 搜索合适的块
    // 1. 在当前 SL 位图里找
    block_header_t *block = NULL;
    uint32_t sl_map = tlsf_inst->sl_bitmap[fl] & (~0U << sl);

    if (!sl_map) {
        // 2. 当前 FL 这一层没有合适的，去更高层(FL)找
        uint32_t fl_map = tlsf_inst->fl_bitmap & (~0U << (fl + 1));
        if (!fl_map) return NULL; // OOM

        fl = tlsf_ffs(fl_map);
        sl_map = tlsf_inst->sl_bitmap[fl]; // 取那层的所有块
    }

    sl = tlsf_ffs(sl_map);
    block = tlsf_inst->blocks[fl][sl];

    // 从空闲链表移除
    remove_free_block(block);

    // 切割 (Split)
    block = block_split(block, adjust_size);

    return (void *)((char *)block + sizeof(block_header_t));
}

void tlsf_free(void *ptr) {
    if (!ptr) return;

    block_header_t *block = (block_header_t *)((char *)ptr - sizeof(block_header_t));

    // 标记为 Free
    block->free_flag = BLOCK_FREE;

    // 物理合并 (Coalesce)
    block = block_merge(block);

    // 插入回空闲链表
    insert_free_block(block);
}

// ================= 调试工具 =================

void debug_dump_ram() {
    printf("\n--- Memory Dump (Physical Order) ---\n");
    block_header_t *curr = (block_header_t *)heap_start;
    int idx = 0;

    while ((void*)curr < (void*)((char*)heap_start + MEM_SIZE)) {
        printf("Block %d: [%p] Size: %8lu | Status: %s | Prev: %p\n",
               idx++,
               curr,
               curr->size,
               curr->free_flag ? "FREE" : "USED",
               curr->phys_prev);

        // 移动到下一个物理块
        curr = (block_header_t *)((char *)curr + curr->size);
        if (curr->size == 0) break; // 防止死循环
    }
    printf("------------------------------------\n");
}

// ================= 主函数：复现你的 2MB 场景 =================

int main() {
    void *memory_pool = malloc(MEM_SIZE);
    tlsf_init(memory_pool, MEM_SIZE);

    debug_dump_ram();

    printf("\n=== 1. Alloc 2MB (Block A) ===\n");
    void *ptrA = tlsf_malloc(2 * 1024 * 1024);
    debug_dump_ram();

    printf("\n=== 2. Alloc 2MB (Block B) ===\n");
    void *ptrB = tlsf_malloc(2 * 1024 * 1024);
    debug_dump_ram();

    printf("\n=== 3. Free Block A (Check Left/Right Merge) ===\n");
    // 此时 Block A 的左边是 NULL，右边是 Block B (USED)。
    // 所以 Free A 后，A 应该独立存在，无法合并。
    tlsf_free(ptrA);
    debug_dump_ram();

    printf("\n=== 4. Free Block B (Check Merge with A) ===\n");
    // 此时 Block B 的左边是 Block A (FREE)，右边是剩余大块 (FREE)。
    // Free B 后，应该发生两次合并：
    // (A + B + 剩余大块) -> 变回最开始的一个完整 128MB
    tlsf_free(ptrB);
    debug_dump_ram();

    return 0;
}