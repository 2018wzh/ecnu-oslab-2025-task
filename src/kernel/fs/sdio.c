/*
    以下内容参考Ailen实现的SD卡驱动, 有一些小的修改
    https://github.com/os-module/visionfive2-sd/tree/main
*/

#include "mod.h"

// 状态检查

static bool can_send_cmd_condition(void)
{
    uint32 cmd_reg = REG32(CMD_REG);
    return !(cmd_reg & (1U << 31)); // start_cmd cleared
}

static bool can_send_data_condition(void)
{
    uint32 status_reg = REG32(STATUS_REG);
    return !STATUS_DATA_BUSY(status_reg); // not data busy
}

static bool response_done_condition(void)
{
    uint32 raw_int_status_reg = REG32(RAW_INT_STATUS_REG);
    uint16 int_status = RAW_INT_INT_STATUS(raw_int_status_reg);
    return RAW_INTR_COMMAND_DONE(int_status);
}

// 带超时检测的状态检查

static bool wait_util_can_send_cmd(void)
{
    uint64 start = timer_get_cycles();
    while (!can_send_cmd_condition()) {
        if (timer_get_cycles() - start >= INTERVAL) {
            printf("wait_util_can_send_cmd: timeout!\n");
            return false;
        }
    }
    return true;
}

static bool wait_util_can_send_data(void)
{
    uint64 start = timer_get_cycles();
    while (!can_send_data_condition()) {
        if (timer_get_cycles() - start >= INTERVAL * 5) {
            printf("wait_util_can_send_data: timeout!\n");
            return false;
        }
    }
    return true;
}

static bool wait_util_response(void)
{
    uint64 start = timer_get_cycles();
    while (!response_done_condition()) {
        if (timer_get_cycles() - start >= INTERVAL) {
            printf("wait_util_response: timeout!\n");
            return false;
        }
    }
    return true;
}

static uint32 fifo_filled_cnt(void)
{
    uint32 status_reg = REG32(STATUS_REG);
    return STATUS_FIFO_COUNT(status_reg);
}

// 核心: 命令发送函数

static bool send_cmd(Cmd cmd_type, CmdReg cmd, uint32 arg, uint8 *data,
                     uint32 data_len, bool is_write, uint32 resp[4])
{
    uint32 raw_int_status;
    uint16 intr;

    // 这里需要一个短暂的时延, 否则写入扇区时会发生RTO超时错误
    uint64 start_time = timer_get_cycles();
    while (timer_get_cycles() - start_time < INTERVAL / 10);

    // 等待上一条命令处理完毕
    if (!wait_util_can_send_cmd())
        return false;

    // 如果是带数据的, 需要等待数据通道空闲
    if (cmd & (1U << 9))
        if (!wait_util_can_send_data())
            return false;

    // 设置ARG寄存器和CMD寄存器
    REG32(ARG_REG) = arg;
    REG32(CMD_REG) = cmd;

    // 等待控制器收到命令
    if (!wait_util_can_send_cmd())
        return false;

    // 如果是需要回复的命令, 等待控制器回应
    if (cmd & (1U << 6))
        if (!wait_util_response())
            return false;

    // 如果是带数据的
    if (cmd & (1U << 9)) {

        uint32 offset = 0;
        volatile uint64 *fifo_ptr = (volatile uint64 *)FIFO_DATA_REG;

        if (!is_write) { // Read from sdcard
            
            while (offset < data_len)
            {
                // 等待RXDR中断
                do {
                    raw_int_status = REG32(RAW_INT_STATUS_REG);
                    intr = RAW_INT_INT_STATUS(raw_int_status);
                    if (raw_interrupt_have_error(intr))
                        goto error;
                    if (RAW_INTR_DTO(intr))
                        break;
                } while (!RAW_INTR_RXDR(intr));

                // 从FIFO里循环读取64bit并逐个字节写入data数组
                while (fifo_filled_cnt() >= 2 && offset < data_len)
                {
                    uint64 fifo_data = *fifo_ptr;
                    for (int i = 0; i < 8 && offset < data_len; i++)
                        data[offset++] = (uint8)(fifo_data >> (i * 8));
                    fifo_ptr++;
                }
            }

        } else { // Write to sdcard

            while (offset < data_len)
            {
                // 等待TXDR中断
                do {
                    raw_int_status = REG32(RAW_INT_STATUS_REG);
                    intr = RAW_INT_INT_STATUS(raw_int_status);
                    if (raw_interrupt_have_error(intr))
                        goto error;
                    if (RAW_INTR_DTO(intr))
                        break;
                } while (!RAW_INTR_TXDR(intr));

                // 每次从data数组里取出64bit写入FIFO
                while (fifo_filled_cnt() < 120 && offset < data_len)
                {
                    uint64 fifo_data = 0;
                    for (int i = 0; i < 8 && offset < data_len; i++)
                        fifo_data |= ((uint64)data[offset++]) << (i * 8);
                    *fifo_ptr = fifo_data;
                    fifo_ptr++;
                }
            }
        }
    }

    // Clear interrupts
    raw_int_status = REG32(RAW_INT_STATUS_REG);
    REG32(RAW_INT_STATUS_REG) = raw_int_status;

    intr = RAW_INT_INT_STATUS(raw_int_status);
    if (raw_interrupt_have_error(intr))
        goto error;

    if (resp) {
        resp[0] = REG32(RESP0_REG);
        resp[1] = REG32(RESP1_REG);
        resp[2] = REG32(RESP2_REG);
        resp[3] = REG32(RESP3_REG);
    }

    return true;

error:
    // 特殊情况处理 (CMD0)
    if (cmd_type == CMD_GO_IDLE_STATE && RAW_INTR_RTO(intr) == 1)
        return true;

    // 输出错误信息
    printf("send_cmd: ERROR CmdTpye = %d, RTO = %d, RE = %d, DRTO = %d, DCRC = %d, SBE = %d, EBE = %d\n",
        cmd_type,
        RAW_INTR_RTO(intr),
        RAW_INTR_RESPONSE_ERR(intr),
        RAW_INTR_DRTO(intr),
        RAW_INTR_DCRC(intr),
        RAW_INTR_SBE(intr),
        RAW_INTR_EBE(intr));

    return false;
}

// 初始化辅助函数

static void set_transaction_size(uint32 blk_size, uint32 byte_count)
{
    REG32(BLK_SIZE_REG) = blk_size;
    REG32(BYTE_CNT_REG) = byte_count;
}

static void reset_clock(void)
{
    // disable clock
    REG32(CLOCK_ENABLE_REG) = 0x0;

    // start_cmd + wait_prvdata_complete + update_clock_registers_only
    CmdReg clock_cmd = (1U << 31) | (1U << 13) | (1U << 21);

    // Reset clock
    send_cmd(111, clock_cmd, 0, NULL, 0, false, NULL);

    // 将时钟周期设为400kHz (low): 3.2MHz / (2 * 4) = 400kHz
    REG32(CLK_DIVIDER_REG) = 0x4;

    // enable clock
    REG32(CLOCK_ENABLE_REG) = 0x1;

    // Reset clock
    send_cmd(111, clock_cmd, 0, NULL, 0, false, NULL);
}

static void reset_fifo(void)
{
    uint32 ctrl_reg = REG32(CTRL_REG);
    ctrl_reg |= (1U << 1); // 启动FIFO模式
    // REG32(FIFO_DATA_REG) = ctrl_reg; // 原版的奇怪操作 (运行不报错)
    REG32(CTRL_REG) = ctrl_reg; // 我觉得合理的写法 (运行也不报错)
}

static void reset_dma(void)
{
    uint32 bus_mode_reg = REG32(BUS_MODE_REG);
    bus_mode_reg |= (1U << 0); // set swr bit
    bus_mode_reg &= ~(1U << 7); // clear de bit
    REG32(BUS_MODE_REG) = bus_mode_reg;

    uint32 ctrl_reg = REG32(CTRL_REG);
    ctrl_reg |= (1U << 2);  // set dma_reset bit
    ctrl_reg &= ~(1U << 25); // clear use_internal_dmac bit
    REG32(CTRL_REG) = ctrl_reg;
}

static void check_version()
{
    uint32 resp[4];
    CmdReg cmd8 = cmd_reg_from_cmd(CMD_SEND_IF_COND);
    if (!send_cmd(CMD_SEND_IF_COND, cmd8, 0x1AA, NULL, 0, false, resp))
        panic("CMD8 fial!");

    if ((resp[0] & 0xAA) == 0) {
        printf("CMD8 echo mismatch: %x\n", resp[0] & 0xFFF);
        panic("Card version: 1.0");
    }
}

static void wait_card_ready(void)
{
    while (1)
    {
        CmdReg cmd55 = cmd_reg_from_cmd(CMD_APP_CMD);
        send_cmd(CMD_APP_CMD, cmd55, 0, NULL, 0, false, NULL);
        
        CmdReg cmd41 = cmd_reg_from_cmd(CMD_SD_SEND_OP_COND);
        uint32 cmd41_arg = (1U << 30) | (1U << 24) | 0xFF8000;
        uint32 resp[4];
        if (!send_cmd(CMD_SD_SEND_OP_COND, cmd41, cmd41_arg, NULL, 0, false, resp))
            panic("cmd41 fail!");
        
        if (resp[0] & (1U << 31))
            break;

        uint64 start = timer_get_cycles();
        while (timer_get_cycles() - start < INTERVAL * 5);
    }
}

static void check_cid(void)
{
    CmdReg cmd2 = cmd_reg_from_cmd(CMD_ALL_SEND_CID);
    uint32 resp[4];
    send_cmd(CMD_ALL_SEND_CID, cmd2, 0, NULL, 0, false, resp);
}

static uint32 get_rca(void)
{
    CmdReg cmd3 = cmd_reg_from_cmd(CMD_SEND_RELATIVE_ADDR);
    uint32 resp[4];
    if (!send_cmd(CMD_SEND_RELATIVE_ADDR, cmd3, 0, NULL, 0, false, resp)) {
        printf("rca = %d\n", resp[0] >> 16);
        panic("get_rca: CMD3 failed");
    }
    return resp[0] >> 16;
}

static void check_csd(uint32 rca)
{
    CmdReg cmd9 = cmd_reg_from_cmd(CMD_SEND_CSD);
    uint32 resp[4];
    if (!send_cmd(CMD_SEND_CSD, cmd9, rca << 16, NULL, 0, false, resp))
        panic("check_csd: CMD9 fail!");
}

static void select_card(uint32 rca)
{
    CmdReg cmd7 = cmd_reg_from_cmd(CMD_SELECT_CARD);
    if (!send_cmd(CMD_SELECT_CARD, cmd7, rca << 16, NULL, 0, false, NULL))
        panic("select_card: CMD7 failed");
}

// static void print_reg_info(void)
// {
//     uint32 reg;

//     reg = REG32(CDETECT_REG);
//     printf("detect = %x\n", reg);

//     reg = REG32(POWER_REG);
//     printf("power = %x\n", reg);

//     reg = REG32(CLOCK_ENABLE_REG);
//     printf("clock_enabel = %x\n", reg);

//     reg = REG32(CTYPE_REG);
//     printf("card_type = %x\n", reg);

//     reg = REG32(CTRL_REG);
//     printf("ctrl = %x\n", reg);

//     reg = REG32(BUS_MODE_REG);
//     printf("bus_mode = %x\n", reg);

//     uint32 dma_desc_base_lower = REG32(DBADDRL_REG);
//     uint32 dma_desc_base_upper = REG32(DBADDRU_REG);
//     uint64 dma_desc_base = ((uint64)dma_desc_base_upper << 32) | dma_desc_base_lower;
//     printf("dma_desc_base = %p\n", dma_desc_base);

//     reg = REG32(CLK_DIVIDER_REG);
//     printf("clock_divider = %p\n", reg);
// }

// === 主初始化流程 ===
void sdio_disk_init(void)
{
    reset_clock();

    reset_fifo();

    REG32(CTYPE_REG) = 0x0; // 启动1bit模式

    reset_dma();

    CmdReg cmd0 = cmd_reg_from_cmd(CMD_GO_IDLE_STATE);
    send_cmd(CMD_GO_IDLE_STATE, cmd0, 0, NULL, 0, false, NULL);

    check_version();

    wait_card_ready();

    check_cid();

    uint32 rca = get_rca();
    // printf("rca = %d\n", rca);

    check_csd(rca);

    // 短暂时延
    uint32 start_time = timer_get_cycles();
    while (timer_get_cycles() - start_time < INTERVAL / 10);

    select_card(rca);

    // print_reg_info();

    uint32 raw_int_status = REG32(RAW_INT_STATUS_REG);
    REG32(RAW_INT_STATUS_REG) = raw_int_status;

    // printf("sdio_disk_init: success!\n");
}

static bool read_sector(uint32 sector, uint8 *buf)
{
    set_transaction_size(SECTOR_SIZE, SECTOR_SIZE);
    CmdReg cmd17 = cmd_reg_from_cmd(CMD_READ_SINGLE_BLOCK);
    return send_cmd(CMD_READ_SINGLE_BLOCK, cmd17, sector, buf, SECTOR_SIZE, false, NULL);
}

static bool write_sector(uint32 sector, uint8 *buf)
{
    set_transaction_size(SECTOR_SIZE, SECTOR_SIZE);
    CmdReg cmd24 = cmd_reg_from_cmd(CMD_WRITE_SINGLE_BLOCK);
    return send_cmd(CMD_WRITE_SINGLE_BLOCK, cmd24, sector, buf, SECTOR_SIZE, true, NULL);
}

// 块设备接口
void sdio_disk_rw(buffer_t *b, bool write)
{
    uint32 sec_per_block = BLOCK_SIZE / SECTOR_SIZE;
    uint64 first_sec = b->block_num * sec_per_block;
    bool ret;

    for (uint32 i = 0; i < sec_per_block; i++) {
        if (write)
            ret = write_sector(first_sec + i, b->data + i * SECTOR_SIZE);
        else
            ret = read_sector(first_sec + i, b->data + i * SECTOR_SIZE);

        if (!ret) {
            printf("sdio_disk_rw: %s sector %llu failed!\n", write ? "write" : "read", first_sec + i);
            panic("sdio_disk_rw: fail!");
        }
    }
}

// 测试函数
#define TEST_BUF_LEN 2048
#define TEST_BUF_SEC (25 * 1024 * 1024 * 2)
uint8 test_buf[TEST_BUF_LEN];

void sdio_disk_test(void)
{
    uint32 test_sector = TEST_BUF_SEC;

    for (uint32 i = 0; i < TEST_BUF_LEN; i++)
        test_buf[i] = (uint8)(i & 0xFF);

    uint32 sec_per_buf = TEST_BUF_LEN / SECTOR_SIZE;
    for (uint32 i = 0; i < sec_per_buf; i++) {
        if (!write_sector(test_sector + i, test_buf + i * SECTOR_SIZE)) {
            printf("sdio_disk_test: write failed at sector %d\n", test_sector + i);
            panic("sdio_disk_test: write fail!");
        }
    }

    for (uint32 i = 0; i < TEST_BUF_LEN; i++)
        test_buf[i] = 0;

    for (uint32 i = 0; i < sec_per_buf; i++) {
        if (!read_sector(test_sector + i, test_buf + i * SECTOR_SIZE)) {
            printf("sdio_disk_test: read failed at sector %d\n", test_sector + i);
            panic("sdio_disk_test: read fail!");
        }
    }

    for (uint32 i = 0; i < TEST_BUF_LEN; i++) {
        uint8 expected = (uint8)(i & 0xFF);
        if (test_buf[i] != expected) {
            printf("sdio_disk_test: data mismatch at byte %d: got %d, expected %d\n",
                   i, test_buf[i], expected);
            panic("sdio_disk_test: data verification fail!");
        }
    }

    // printf("sdio_disk_test: success!\n");
}