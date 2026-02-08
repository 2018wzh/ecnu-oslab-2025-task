#include "mod.h"

// 全局系统时钟
static timer_t sys_timer;

// 时钟创建
void timer_create()
{
    sys_timer.ticks = 0;
    spinlock_init(&sys_timer.lk, "sys_timer");
}

// 返回系统启动至今经历了多少个cycles
uint64 timer_get_cycles()
{
    uint64 cycles;
    asm volatile("rdtime %0" : "=r"(cycles));
    return cycles;
}

// 时钟更新
void timer_update()
{
    // 设置下一次时钟中断的触发位置
    SBI_SET_TIMER(timer_get_cycles() + INTERVAL);

    // 更新ticks记数并唤醒等待sys_timer的进程
    int main_cpuid;

#ifdef VISIONFIVE2
    main_cpuid = 1; // 开发板的hart0是不启用的，需要特殊处理
#else
    main_cpuid = 0;
#endif

    if (mycpuid() == main_cpuid) {
        spinlock_acquire(&sys_timer.lk);
        sys_timer.ticks++;
        proc_wakeup(&sys_timer);
        spinlock_release(&sys_timer.lk);
    }
}

// 获取滴答数量
uint64 timer_get_ticks()
{
    return sys_timer.ticks;
}

// 让进程睡眠ntick个时钟周期
void timer_wait(uint64 ntick)
{
    uint64 tar_ticks;

    spinlock_acquire(&sys_timer.lk);
    tar_ticks = ntick + sys_timer.ticks;
    while (sys_timer.ticks < tar_ticks)
        proc_sleep(&sys_timer, &sys_timer.lk);
    spinlock_release(&sys_timer.lk);
}