# 引入通用配置文件
include common.mk

# 配置CPU核心数量
CPUNUM = 2

# 定义目标文件输出目录
TARGET = target

# 定义各模块路径
KernelPath = src/kernel
UserPath   = src/user
MKFSPath   = src/mkfs
LoaderPath = src/loader

# 链接脚本
KERNEL_QEMU_LD = $(LoaderPath)/kernel_qemu.ld
KERNEL_VF2_LD = $(LoaderPath)/kernel_vf2.ld
USER_LD   = $(LoaderPath)/user.ld

# 定义目标文件路径
ELFKernelQEMU = $(TARGET)/kernel/kernel_qemu.elf
ELFKernelVF2 = $(TARGET)/kernel/kernel_vf2.elf
NakedKernel = $(TARGET)/kernel/kernel_vf2.bin
ELFUserInit = $(UserPath)/initcode.h
DISKIMG     = $(TARGET)/mkfs/disk.img

# 目标SD卡即内核镜像偏移量 (开发板模式使用)
SDcard = /dev/sdb     # SD卡的设备地址, 按照实际情况修改 (如果SD卡内有文件系统, 建议先全盘格式化)
KERNEL_OFFSET = 20480 # 前20GB供disk.img使用, 从这里开始存放kernel.bin (bs=1M)

# 收集内核源文件 (.c .S，包括子目录)
KernelSourceFile = $(wildcard $(KernelPath)/*.c) $(wildcard $(KernelPath)/*.S)
KernelSourceFile += $(wildcard $(KernelPath)/*/*.c) $(wildcard $(KernelPath)/*/*.S)

# 内核目标文件
KernelOBJ = $(patsubst $(KernelPath)/%.S, $(TARGET)/kernel/%.o, $(filter %.S, $(KernelSourceFile)))
KernelOBJ += $(patsubst $(KernelPath)/%.c, $(TARGET)/kernel/%.o, $(filter %.c, $(KernelSourceFile)))

# 用户程序分类: 启动程序(initcode.c) 通用库(syscall.c、help.c) 测试程序(test_1.c test_2.c ...)
USER_INIT_C = $(UserPath)/initcode.c
USER_LIB_C = $(UserPath)/syscall.c $(UserPath)/help.c
USER_TEST_C = $(filter-out $(USER_INIT_C) $(USER_LIB_C), $(wildcard $(UserPath)/*.c))

# 用户目标文件
USER_INIT_OBJ = $(TARGET)/user/initcode.o
USER_LIB_OBJ  = $(USER_LIB_C:$(UserPath)/%.c=$(TARGET)/user/%.o)
USER_TEST_OBJ = $(USER_TEST_C:$(UserPath)/%.c=$(TARGET)/user/%.o)
USER_TEST_ELF = $(USER_TEST_C:$(UserPath)/%.c=$(TARGET)/user/%.elf)

.SECONDARY: $(USER_LIB_OBJ) $(USER_TEST_OBJ)

# QEMU 模拟器配置
QEMU     = qemu-system-riscv64
QEMUOPTS = -machine virt -bios default -kernel $(ELFKernelQEMU)
QEMUOPTS += -m 128M -smp $(CPUNUM) -nographic
QEMUOPTS += -drive file=$(DISKIMG),if=none,format=raw,id=x0
QEMUOPTS += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0

# 调试配置
GDBPORT = $(shell expr `id -u` % 5000 + 25000)
QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
	then echo "-gdb tcp::$(GDBPORT)"; \
	else echo "-s -p $(GDBPORT)"; fi)

# 生成 .gdbinit
.gdbinit: .gdbinit.tmpl-riscv
	sed "s/:1234/:$(GDBPORT)/" < $^ > $@

# 创建输出目录
.PHONY: $(TARGET)
$(TARGET):
ifeq ($(wildcard $(TARGET)),)
	@mkdir -p $(TARGET)/kernel
	@mkdir -p $(TARGET)/kernel/arch
	@mkdir -p $(TARGET)/kernel/boot
	@mkdir -p $(TARGET)/kernel/lock
	@mkdir -p $(TARGET)/kernel/lib
	@mkdir -p $(TARGET)/kernel/mem
	@mkdir -p $(TARGET)/kernel/trap
	@mkdir -p $(TARGET)/kernel/proc
	@mkdir -p $(TARGET)/kernel/syscall
	@mkdir -p $(TARGET)/kernel/fs
	@mkdir -p $(TARGET)/user
	@mkdir -p $(TARGET)/mkfs
endif

# 内核编译规则
$(TARGET)/kernel/%.o: $(KernelPath)/%.S
	$(CC) $(CFLAGS) -c -o $@ $<

$(TARGET)/kernel/%.o: $(KernelPath)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# 用户程序编译规则

# initcode.o（特殊）
$(USER_INIT_OBJ): $(USER_INIT_C)
	$(CC) $(CFLAGS) -I. -c -o $@ $<

# 通用库和测试程序的 .o（共享规则）
$(TARGET)/user/%.o: $(UserPath)/%.c
	$(CC) $(CFLAGS) -I. -c -o $@ $<

# 链接普通用户程序
$(TARGET)/user/%.elf: $(TARGET)/user/%.o $(USER_LIB_OBJ)
	$(LD) $(LDFLAGS) -T $(USER_LD) $< $(USER_LIB_OBJ) -o $@

# 生成 initcode.h（供内核嵌入）
$(ELFUserInit): $(USER_INIT_OBJ)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $(TARGET)/user/initcode.out $<
	$(OBJCOPY) -S -O binary $(TARGET)/user/initcode.out $(TARGET)/user/initcode
	xxd -i $(TARGET)/user/initcode > $(ELFUserInit)

# 生成 kernel_qemu.elf (供QEMU使用)
$(ELFKernelQEMU): $(KernelOBJ)
	$(LD) $(LDFLAGS) -T $(KERNEL_QEMU_LD) $^ -o $@

# 生成 kernel_vf2.elf (供开发板使用)
$(ELFKernelVF2): $(KernelOBJ)
	$(LD) $(LDFLAGS) -T $(KERNEL_VF2_LD) $^ -o $@

# 生成 kernel_vf2.bin (供开发板使用)
$(NakedKernel): $(ELFKernelVF2)
	$(OBJCOPY) -O binary $< $@

# 生成磁盘映像（包含所有普通用户程序）
$(DISKIMG): $(USER_TEST_ELF)
	gcc -Werror -Wall -I. -o $(TARGET)/mkfs/mkfs $(MKFSPath)/mkfs.c
	$(TARGET)/mkfs/mkfs $@ $(USER_TEST_ELF)

# 构建目标 (QEMU)
.PHONY: qemu
qemu: $(TARGET) $(ELFUserInit) $(USER_TEST_ELF) $(ELFKernelQEMU) $(DISKIMG)
	@echo "===== make qemu success! ====="

# 执行内核 (QEMU)
.PHONY: run
run: qemu
	$(QEMU) $(QEMUOPTS)

# 构建目标 (开发板)
.PHONY: vf2
vf2: CFLAGS += -DVISIONFIVE2 # 相当于 define VISIONFIVE2 启动条件编译
vf2: $(TARGET) $(ELFUserInit) $(USER_TEST_ELF) $(ELFKernelVF2) $(NakedKernel) $(DISKIMG)
	@echo "===== make visionfive2 success! ====="

# 准备包含文件系统初始状态和内核镜像的SD卡 (开发板)
# SD卡布局: [disk.img] [空闲] [kernel_vf2.bin] [空闲]
.PHONY: sdcard
sdcard: vf2
	sudo dd if=$(DISKIMG) of=$(SDcard) bs=1M conv=fsync status=progress
	sudo dd if=$(NakedKernel) of=$(SDcard) bs=1M seek=$(KERNEL_OFFSET) conv=fsync status=progress
	sync
	@echo "===== prepare sdcard success! ======"

# SD卡准备完成后的操作
# 1. 将SD卡插入开发板, 使用TTL-to-USB转接器连接开发板的GPIO与主机, 通过putty.exe建立串口连接
# 2. 连接开发板电源, 以1-bit QSPI Nor Flash模式(默认)启动开发板(连续点击两次重启键)
# 3. 等待Uboot和OpenSBI完成一系列初始化(能看到一些输出, 大约15s后进入类似shell的地方)
# 2. 输入 “mmc dev 1” 切换到SD卡
# 3. 输入 "mmc read 40200000 2800000 1000" 加载SD卡中的kernel.bin到目标内存位置
#    注: 0x40200000对应约定的内核启动位置 0x2800000对应20GB(KENEL_OFFSET) 0x1000对应4096个扇区(kernel_vf2.bin只有50KB, 足够大)
# 4. 输入 "go 40200000" 启动内核

# 调试目标
.PHONY: debug
debug: qemu .gdbinit
	$(QEMU) $(QEMUOPTS) -S $(QEMUGDB)

# 清理目标
.PHONY: clean
clean:
	rm -rf $(TARGET)
	rm -f $(UserPath)/initcode.h
	rm -f .gdbinit
