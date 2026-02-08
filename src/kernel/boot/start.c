#include "../arch/mod.h"
#include "../trap/mod.h"

// 每个CPU在运行操作系统时需要一个初始的函数栈
__attribute__((aligned(16))) uint8 CPU_stack[4096 * N_CPU];

extern void main();

static void trap_handler_loop()
{
    while (1);    
}

// OpenSBI会传递hartid和设备树信息
void start(uint64 hartid, uint64 dtb_entry)
{
    // 暂时不开启分页，使用物理地址
    w_satp(0);

    // 将hartid存储到tp寄存器中
    w_tp(hartid);

    // 打开S-mode所有中断
    w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);

    // 暂时将trap响应程序设为死循环, 等待trap初始化
    w_stvec((uint64)trap_handler_loop);

    // 直接进入main函数
    main();
}