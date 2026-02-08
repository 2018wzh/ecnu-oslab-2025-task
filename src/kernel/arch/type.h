#pragma once

/*------------------- 基本类型定义 -------------------*/

typedef char int8;
typedef short int16;
typedef int int32;
typedef long long int64;
typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int uint32;
typedef unsigned long long uint64;
typedef unsigned long long reg;

typedef enum
{
    false = 0,
    true = 1
} bool;

#ifndef NULL
#define NULL ((void *)0)
#endif

// 开发板: hart[0]有其他作用, hart[1-4]运行OS内核
// QEMU: 只启动2个hart, 便于调试

#ifdef VISIONFIVE2
#define N_CPU 5
#else
#define N_CPU 2
#endif

/*------------------- RISC-V架构常量与宏定义 -------------------*/

/* Machine Status Register (mstatus) */
#define MSTATUS_MPP_MASK (3L << 11)
#define MSTATUS_MPP_M (3L << 11)
#define MSTATUS_MPP_S (1L << 11)
#define MSTATUS_MPP_U (0L << 11)
#define MSTATUS_MIE (1L << 3)

/* Supervisor Status Register (sstatus) */
#define SSTATUS_SPP (1L << 8)
#define SSTATUS_SPIE (1L << 5)
#define SSTATUS_UPIE (1L << 4)
#define SSTATUS_SIE (1L << 1)
#define SSTATUS_UIE (1L << 0)

/* Supervisor Interrupt Enable (sie) */
#define SIE_SEIE (1L << 9) // S-mode 外设中断
#define SIE_STIE (1L << 5) // S-mode 时钟中断
#define SIE_SSIE (1L << 1) // S-mode 软件中断

/* Machine-mode Interrupt Enable (mie) */
#define MIE_MEIE (1L << 11) // M-mode 外设中断
#define MIE_MTIE (1L << 7)  // M-mode 时钟中断
#define MIE_MSIE (1L << 3)  // M-mode 软件中断

/*------------------- OpenSBI相关 -------------------*/

struct sbiret {
	uint64 error;
	uint64 value;
};

/* 1.串口能力: 字符级输入输出 */

#define SBI_LEGACY_ECALL(__num, __a0, __a1, __a2)                                                  \
	({                                                                                         \
		register unsigned long a0 asm("a0") = (unsigned long)(__a0);                       \
		register unsigned long a1 asm("a1") = (unsigned long)(__a1);                       \
		register unsigned long a2 asm("a2") = (unsigned long)(__a2);                       \
		register unsigned long a7 asm("a7") = (unsigned long)(__num);                      \
		asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");           \
		a0;                                                                                \
	})

#define SBI_LEGACY_PUTCHAR_NUM 0x01
#define SBI_LEGACY_GETCHAR_NUM 0x02

// 阻塞式: 直到字符被成功输出到串口才会返回
#define SBI_PUTCHAR(ch) SBI_LEGACY_ECALL(SBI_LEGACY_PUTCHAR_NUM, ch, 0, 0)

// 非阻塞式: 如果没有读到字符，会直接返回255; 如果读到字符则返回字符(char)
#define SBI_GETCHAR() SBI_LEGACY_ECALL(SBI_LEGACY_GETCHAR_NUM, 0, 0, 0)


/* 2.其他能力: 时钟 刷新TLB hart启动与关闭 */

#define SBI_ECALL(__eid, __fid, __a0, __a1, __a2, __a3, __a4)                                      \
	({                                                                                         \
		unsigned long error = 0, value = 0;                                                \
		register unsigned long a0 asm("a0") = (unsigned long)(__a0);                       \
		register unsigned long a1 asm("a1") = (unsigned long)(__a1);                       \
		register unsigned long a2 asm("a2") = (unsigned long)(__a2);                       \
		register unsigned long a3 asm("a3") = (unsigned long)(__a3);                       \
		register unsigned long a4 asm("a4") = (unsigned long)(__a4);                       \
		register unsigned long a6 asm("a6") = (unsigned long)(__fid);                      \
		register unsigned long a7 asm("a7") = (unsigned long)(__eid);                      \
		asm volatile("ecall\n"                                                             \
			     "mv %[error], a0\n"                                                   \
			     "mv %[value], a1\n"                                                   \
			     : "+r"(a0), "+r"(a1), [error] "=r"(error), [value] "=r"(value)        \
			     : "r"(a2), "r"(a3), "r"(a4), "r"(a6), "r"(a7)                         \
			     : "memory");                                                          \
		(struct sbiret){error, value};                                                     \
	})

#define SBI_TIMER_EID 0x54494D45
#define SBI_SET_TIMER_FID 0
#define SBI_RFENCE_EID 0x52464E43
#define SBI_RFENCE_SFENCE_VMA_FID 1
#define SBI_HSM_EID 0x48534D
#define SBI_HSM_HART_START_FID 0
#define SBI_HSM_HART_STOP_FID 1
#define SBI_SYSTEM_RESET_EID 0x53525354
#define SBI_SYSTEM_RESET_FID 0

// 设置计时器 (返回sbiret)
#define SBI_SET_TIMER(stime_value)                                                                 \
	SBI_ECALL(SBI_TIMER_EID, SBI_SET_TIMER_FID, stime_value, 0, 0, 0, 0)

// 刷新TLB (返回sbiret)
#define SBI_RFENCE_SFENCE_VMA(hart_mask, hart_mask_base, start_addr, size)                         \
	SBI_ECALL(SBI_RFENCE_EID, SBI_RFENCE_SFENCE_VMA_FID, hart_mask, hart_mask_base,                \
		  start_addr, size, 0)

// 启动指定hart (返回sbiret)
#define SBI_HART_START(hartid, start_addr, opaque)                                                 \
	SBI_ECALL(SBI_HSM_EID, SBI_HSM_HART_START_FID, hartid, start_addr, opaque, 0, 0)

// 关闭当前hart (返回sbiret)
#define SBI_HART_STOP()                                                                            \
	SBI_ECALL(SBI_HSM_EID, SBI_HSM_HART_STOP_FID, 0, 0, 0, 0, 0)

// 重启或关闭整个系统 (返回sbiret)
#define SBI_SYSTEM_RESET(reset_type, reset_reason)                                                 \
	SBI_ECALL(SBI_SYSTEM_RESET_EID, SBI_SYSTEM_RESET_FID, reset_type, reset_reason, 0, 0, 0)
