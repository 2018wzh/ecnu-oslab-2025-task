#include "mod.h"

// PLIC初始化
void plic_init()
{
// 开发板用不到则两个设备中断, 而是用忙等方法驱动UART和SDIO
#ifndef VISIONFIVE2
    // 设置UART和VIRTIO中断优先级
    *(uint32*)(PLIC_PRIORITY(UART_IRQ)) = 1;
    *(uint32*)(PLIC_PRIORITY(VIRTIO_IRQ)) = 1;
#endif
}

// PLIC核心初始化
void plic_inithart()
{
// 开发板用不到则两个设备中断, 而是用忙等方法驱动UART和SDIO
    int hartid = mycpuid();
#ifndef VISIONFIVE2
    // 使能中断开关
    *(uint32 *)PLIC_SENABLE(hartid) = ((1 << UART_IRQ) | (1 << VIRTIO_IRQ));
#endif
    // 设置响应阈值
    *(uint32 *)PLIC_SPRIORITY(hartid) = 0;
}

// 获取中断号
int plic_claim(void)
{
    int hartid = mycpuid();
    int irq = *(uint32 *)PLIC_SCLAIM(hartid);
    return irq;
}

// 确认该中断号对应中断已经完成
void plic_complete(int irq)
{
    int hartid = mycpuid();
    *(uint32 *)PLIC_SCLAIM(hartid) = irq;
}