#include <stdint.h>
#include <stdio.h>

// ==========================================
// 1. 定义模拟的硬件地址
// ==========================================
// volatile 告诉编译器不要优化，每次都要去内存读，因为硬件可能会改变它
volatile int HARDWARE_A = 0; 
volatile int HARDWARE_B = 0;

// 假设硬件发出的“就绪”信号是 0xFF
#define SIGNAL_READY 0xFF 

// ==========================================
// 2. 线程上下文结构体 (TCB)
// ==========================================
typedef struct {
    char name;              // 线程名字
    int current_step;       // 核心：记录当前执行到第几步 (0-4)
    volatile int* addr;     // 监控的地址
    int is_finished;        // 是否全部完成
} ThreadContext;

// ==========================================
// 3. 线程逻辑 (状态机实现)
// ==========================================
void thread_task(ThreadContext* ctx) {
    if (ctx->is_finished) {
        return;
    }

    // 核心逻辑：根据 current_step 决定跳转到哪里执行
    // 这实现了“下一次还得从同样的地方开始”
    switch (ctx->current_step) {
        
        case 0: // 等待状态 1
            if (*(ctx->addr) == SIGNAL_READY) {
                // 1. 执行动作：写 1
                *(ctx->addr) = 1; 
                printf("[%c] Detect Ready -> Wrote 1\n", ctx->name);
                
                // 2. 更新状态：下次来执行 case 1
                ctx->current_step = 1; 
            } else {
                // 3. 等待不到：直接退出 (Yield)，下次进函数还会走 case 0
                return; 
            }
            break;

        case 1: // 等待状态 2
            // 这里假设写完1后，硬件清零了，再次发出 0xFF 代表第二次就绪
            if (*(ctx->addr) == SIGNAL_READY) {
                *(ctx->addr) = 2; // 写 2
                printf("[%c] Detect Ready -> Wrote 2\n", ctx->name);
                ctx->current_step = 2; // 推进到下一步
            } else {
                return; // 退出，保留现场
            }
            break;

        case 2: // 等待状态 3
            if (*(ctx->addr) == SIGNAL_READY) {
                *(ctx->addr) = 3;
                printf("[%c] Detect Ready -> Wrote 3\n", ctx->name);
                ctx->current_step = 3;
            } else {
                return;
            }
            break;

        case 3: // 等待状态 4
            if (*(ctx->addr) == SIGNAL_READY) {
                *(ctx->addr) = 4;
                printf("[%c] Detect Ready -> Wrote 4\n", ctx->name);
                ctx->current_step = 4;
            } else {
                return;
            }
            break;

        case 4: // 等待状态 5
            if (*(ctx->addr) == SIGNAL_READY) {
                *(ctx->addr) = 5;
                printf("[%c] Detect Ready -> Wrote 5\n", ctx->name);
                // 全部完成
                ctx->is_finished = 1;
                printf("[%c] Task Completed!\n", ctx->name);
            } else {
                return;
            }
            break;
            
        default:
            ctx->is_finished = 1;
            break;
    }
}

// ==========================================
// 4. 模拟外部硬件行为 (为了让程序跑起来)
// ==========================================
void simulate_hardware_events() {
    // 简单的模拟：如果发现内存里是被线程写过的值(1,2,3..)，就重置为 READY
    // 模拟硬件“收到数据处理完，请求下一个数据”
    
    if (HARDWARE_A > 0 && HARDWARE_A <= 5 && HARDWARE_A != SIGNAL_READY) {
        printf("   [HW-A] Ack %d, Requesting Next...\n", HARDWARE_A);
        HARDWARE_A = SIGNAL_READY; 
    }
    if (HARDWARE_B > 0 && HARDWARE_B <= 5 && HARDWARE_B != SIGNAL_READY) {
        printf("   [HW-B] Ack %d, Requesting Next...\n", HARDWARE_B);
        HARDWARE_B = SIGNAL_READY;
    }
}

// ==========================================
// 5. 主调度器 (Main Loop)
// ==========================================
int main() {
    // 初始化上下文
    ThreadContext t_a = { 'A', 0, &HARDWARE_A, 0 };
    ThreadContext t_b = { 'B', 0, &HARDWARE_B, 0 };

    // 初始状态：硬件准备好了
    HARDWARE_A = SIGNAL_READY;
    HARDWARE_B = SIGNAL_READY;

    printf("System Start.\n");

    // 死循环调度 (Round-Robin)
    while (1) {
        
        // 调度 线程 A
        thread_task(&t_a);
        
        // 调度 线程 B
        thread_task(&t_b);

        // 模拟硬件中断/响应 (仅为了演示，实际裸机中这是硬件自己变的)
        simulate_hardware_events();

        // 检查是否都结束了以退出演示
        if (t_a.is_finished && t_b.is_finished) break;
        
        // 在真实裸机中，这里通常会有一个 __WFI() (Wait For Interrupt) 
        // 或者简单的延时，避免空转烧CPU
    }

    return 0;
}