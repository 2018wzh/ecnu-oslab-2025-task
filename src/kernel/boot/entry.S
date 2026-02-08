# 内核入口
# 注意: 使用OpenSBI后此时处于S-mode, 不能使用M-mode的寄存器操作
.section .text
.global _entry
_entry:
        # CPU_stack 定义于start.c中
        # sp = CPU_stack + ((hartid + 1) * 4096)
        # 将sp置于当前CPU的内核栈的栈顶
        la sp, CPU_stack
        li t0, 4096
        mv t1, a0
        addi t1, t1, 1
        mul t0, t0, t1
        add sp, sp, t0
        # 跳转到start
        call start
spin:
        j spin
