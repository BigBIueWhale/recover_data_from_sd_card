/*******************************************************************************
 * nand_dump.c
 * Bare-metal Zynq ARM program for NAND flash dump via the FPGA controller
 *
 * Runs on the Zynq PS (Cortex-A9). Communicates with the NAND controller
 * in the PL via memory-mapped AXI registers at the GP0 base address.
 *
 * Protocol over UART (115200 8N1 by default, increase to 921600 for dump):
 *   Host sends single-character commands:
 *     'R' - NAND Reset
 *     'I' - Read ID (returns 5 bytes)
 *     'S' - Read Status (returns 1 byte)
 *     'P' - Read Parameter Page (returns 256 bytes)
 *     'D' - Dump all pages (streams raw NAND content)
 *     'G' - Read single page (address set by 'A' command)
 *     'A' - Set address: followed by 5 bytes (col_lo, col_hi, row0, row1, row2)
 *     'C' - Set read count: followed by 2 bytes (count_lo, count_hi)
 *     'V' - Version check
 *
 * Build: Compile with Xilinx Vitis / xsct bare-metal BSP for Zynq.
 *
 * Target: Digilent Arty Z7 (Zynq XC7Z020)
 ******************************************************************************/

#include <stdint.h>
#include "xil_io.h"        /* Xil_In32, Xil_Out32 */
#include "xparameters.h"   /* XPAR_ base addresses */
#include "xuartps.h"       /* PS UART driver */
#include "sleep.h"         /* usleep */

/*---------------------------------------------------------------------------
 * AXI register base address (Zynq GP0 default: 0x40000000 or as configured)
 * Adjust NAND_BASE to match your Vivado address map.
 *---------------------------------------------------------------------------*/
#ifndef NAND_BASE
#define NAND_BASE  0x40000000U
#endif

/* Register offsets */
#define REG_CTRL       0x0000
#define REG_STATUS     0x0004
#define REG_ADDR_COL   0x0008
#define REG_ADDR_ROW   0x000C
#define REG_RD_COUNT   0x0010
#define REG_ID_LO      0x0014
#define REG_ID_HI      0x0018
#define REG_NAND_STAT  0x001C
#define REG_PAGE_IDX   0x0020
#define REG_VERSION    0x0024
#define REG_PAGE_BUF   0x4000  /* page buffer base */

/* Operation codes (bits [3:1] of CTRL register) */
#define OP_RESET       (1 << 1)
#define OP_READ_ID     (2 << 1)
#define OP_READ_STATUS (3 << 1)
#define OP_READ_PAGE   (4 << 1)
#define OP_READ_PARAM  (5 << 1)

/* CTRL register bits */
#define CTRL_START     (1 << 0)
#define CTRL_CLR_DONE  (1 << 4)

/* STATUS register bits */
#define STATUS_BUSY    (1 << 0)
#define STATUS_DONE    (1 << 1)

/*---------------------------------------------------------------------------
 * Helper functions
 *---------------------------------------------------------------------------*/
static inline void nand_write(uint32_t offset, uint32_t val)
{
    Xil_Out32(NAND_BASE + offset, val);
}

static inline uint32_t nand_read(uint32_t offset)
{
    return Xil_In32(NAND_BASE + offset);
}

static void nand_start_op(uint32_t op_bits)
{
    /* Clear previous done flag */
    nand_write(REG_CTRL, CTRL_CLR_DONE);
    /* Start operation */
    nand_write(REG_CTRL, op_bits | CTRL_START);
}

static int nand_wait_done(uint32_t timeout_ms)
{
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        uint32_t st = nand_read(REG_STATUS);
        if (st & STATUS_DONE)
            return 0;  /* success */
        usleep(100);
        elapsed++;
    }
    return -1;  /* timeout */
}

/*---------------------------------------------------------------------------
 * UART I/O (PS UART, directly via register access for bare-metal)
 *---------------------------------------------------------------------------*/
static XUartPs uart;

static void uart_init(void)
{
    XUartPs_Config *cfg = XUartPs_LookupConfig(XPAR_XUARTPS_0_DEVICE_ID);
    XUartPs_CfgInitialize(&uart, cfg, cfg->BaseAddress);
    XUartPs_SetBaudRate(&uart, 921600);
}

static void uart_send_byte(uint8_t b)
{
    while (XUartPs_IsTransmitFull(uart.Config.BaseAddress))
        ;
    XUartPs_SendByte(uart.Config.BaseAddress, b);
}

static void uart_send_buf(const uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        uart_send_byte(buf[i]);
}

static uint8_t uart_recv_byte(void)
{
    return XUartPs_RecvByte(uart.Config.BaseAddress);
}

static void uart_send_str(const char *s)
{
    while (*s)
        uart_send_byte((uint8_t)*s++);
}

static void uart_send_hex32(uint32_t val)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4)
        uart_send_byte(hex[(val >> i) & 0xF]);
}

/*---------------------------------------------------------------------------
 * NAND operations
 *---------------------------------------------------------------------------*/
static void do_reset(void)
{
    nand_start_op(OP_RESET);
    if (nand_wait_done(2000) == 0)
        uart_send_str("OK:RESET\r\n");
    else
        uart_send_str("ERR:RESET_TIMEOUT\r\n");
}

static void do_read_id(void)
{
    nand_write(REG_ADDR_COL, 0x0000);  /* address byte = 0x00 */
    nand_write(REG_RD_COUNT, 5);
    nand_start_op(OP_READ_ID);
    if (nand_wait_done(1000) == 0) {
        uint32_t id_lo = nand_read(REG_ID_LO);
        uint32_t id_hi = nand_read(REG_ID_HI);
        uart_send_str("ID:");
        uart_send_hex32(id_lo);
        uart_send_byte(' ');
        uart_send_hex32(id_hi);
        uart_send_str("\r\n");

        /* Decode common fields */
        uint8_t maker  = id_lo & 0xFF;
        uint8_t device = (id_lo >> 8) & 0xFF;
        uart_send_str("  Maker=0x");
        uart_send_hex32(maker);
        uart_send_str(" Device=0x");
        uart_send_hex32(device);
        uart_send_str("\r\n");

        /* Decode page/block size from byte 3 (ONFI convention) */
        uint8_t byte3 = (id_lo >> 24) & 0xFF;
        uint32_t page_size = 1024 << (byte3 & 0x03);
        uint32_t block_pages = 64 << ((byte3 >> 4) & 0x03);
        uint32_t spare_per_512 = (byte3 & 0x04) ? 16 : 8;
        uart_send_str("  PageSize=");
        uart_send_hex32(page_size);
        uart_send_str(" BlockPages=");
        uart_send_hex32(block_pages);
        uart_send_str(" Spare/512=");
        uart_send_hex32(spare_per_512);
        uart_send_str("\r\n");
    } else {
        uart_send_str("ERR:ID_TIMEOUT\r\n");
    }
}

static void do_read_status(void)
{
    nand_start_op(OP_READ_STATUS);
    if (nand_wait_done(1000) == 0) {
        uint32_t st = nand_read(REG_NAND_STAT);
        uart_send_str("NAND_STATUS=0x");
        uart_send_hex32(st & 0xFF);
        uart_send_str("\r\n");
    } else {
        uart_send_str("ERR:STATUS_TIMEOUT\r\n");
    }
}

static void do_read_page(void)
{
    uint32_t rd_count = nand_read(REG_RD_COUNT);
    nand_start_op(OP_READ_PAGE);
    if (nand_wait_done(5000) == 0) {
        uint32_t bytes_read = nand_read(REG_PAGE_IDX);
        uart_send_str("PAGE_OK bytes=");
        uart_send_hex32(bytes_read);
        uart_send_str("\r\n");

        /* Send raw page data as binary (prefixed with 4-byte length) */
        uint8_t hdr[4];
        hdr[0] = (bytes_read >>  0) & 0xFF;
        hdr[1] = (bytes_read >>  8) & 0xFF;
        hdr[2] = (bytes_read >> 16) & 0xFF;
        hdr[3] = (bytes_read >> 24) & 0xFF;
        uart_send_buf(hdr, 4);

        /* Stream page buffer words */
        uint32_t words = (bytes_read + 3) / 4;
        for (uint32_t i = 0; i < words; i++) {
            uint32_t w = nand_read(REG_PAGE_BUF + i * 4);
            uint8_t b[4];
            b[0] = (w >>  0) & 0xFF;
            b[1] = (w >>  8) & 0xFF;
            b[2] = (w >> 16) & 0xFF;
            b[3] = (w >> 24) & 0xFF;
            /* Only send the valid bytes in the last word */
            uint32_t remaining = bytes_read - i * 4;
            uint32_t send_cnt = remaining >= 4 ? 4 : remaining;
            uart_send_buf(b, send_cnt);
        }
    } else {
        uart_send_str("ERR:PAGE_TIMEOUT\r\n");
    }
}

static void do_dump_all(void)
{
    /*
     * Full NAND dump: iterate over all pages and stream data.
     *
     * First, read the NAND ID to determine geometry. Then loop over all
     * blocks and pages, reading each page and sending the raw data over UART.
     *
     * The host receiver script should be ready to capture the binary stream.
     *
     * Protocol for dump:
     *   ARM sends "DUMP_START\r\n"
     *   For each page:
     *     ARM sends 4 bytes: page_bytes (little-endian uint32)
     *     ARM sends 3 bytes: row address (little-endian uint24)
     *     ARM sends page_bytes of raw data
     *   ARM sends "DUMP_END\r\n" when complete
     */

    /* Read ID to determine geometry */
    nand_write(REG_ADDR_COL, 0x0000);
    nand_write(REG_RD_COUNT, 5);
    nand_start_op(OP_READ_ID);
    if (nand_wait_done(1000) != 0) {
        uart_send_str("ERR:DUMP_ID_FAIL\r\n");
        return;
    }

    uint32_t id_lo = nand_read(REG_ID_LO);
    uint8_t byte3 = (id_lo >> 24) & 0xFF;
    uint8_t byte4 = nand_read(REG_ID_HI) & 0xFF;

    /* Decode geometry from ID bytes (ONFI convention) */
    uint32_t page_data_size = 1024U << (byte3 & 0x03);
    uint32_t spare_per_512  = (byte3 & 0x04) ? 16 : 8;
    uint32_t spare_total    = (page_data_size / 512) * spare_per_512;
    uint32_t page_total     = page_data_size + spare_total;
    uint32_t pages_per_block = 64U << ((byte3 >> 4) & 0x03);

    /* Plane count and die info from byte4 */
    uint32_t plane_count = 1U << ((byte4 >> 2) & 0x03);

    /* Estimate total blocks — for a 64 GB die, typical configurations:
     * 8KB page, 256 pages/block = 2MB/block, 32768 blocks
     * This needs to be adjusted for the actual NAND. The user should
     * override via the 'A' and 'C' commands or modify this code. */
    uint32_t total_blocks = 4096;  /* CONSERVATIVE DEFAULT — adjust! */

    uart_send_str("DUMP_START\r\n");
    uart_send_str("  page_data=");
    uart_send_hex32(page_data_size);
    uart_send_str(" spare=");
    uart_send_hex32(spare_total);
    uart_send_str(" pages/blk=");
    uart_send_hex32(pages_per_block);
    uart_send_str(" blocks=");
    uart_send_hex32(total_blocks);
    uart_send_str("\r\n");

    /* Set read count for full page + spare */
    nand_write(REG_RD_COUNT, page_total);

    for (uint32_t block = 0; block < total_blocks; block++) {
        for (uint32_t page = 0; page < pages_per_block; page++) {
            uint32_t row = block * pages_per_block + page;

            /* Set address: column = 0 (read from start of page) */
            nand_write(REG_ADDR_COL, 0x0000);
            nand_write(REG_ADDR_ROW, row & 0x00FFFFFF);

            nand_start_op(OP_READ_PAGE);

            if (nand_wait_done(10000) != 0) {
                /* Timeout — send error marker and continue */
                uint8_t marker[7] = {0xFF, 0xFF, 0xFF, 0xFF, 'E', 'R', 'R'};
                uart_send_buf(marker, 7);
                continue;
            }

            uint32_t bytes_read = nand_read(REG_PAGE_IDX);

            /* Send header: page_bytes (4) + row_addr (3) */
            uint8_t hdr[7];
            hdr[0] = (bytes_read >>  0) & 0xFF;
            hdr[1] = (bytes_read >>  8) & 0xFF;
            hdr[2] = (bytes_read >> 16) & 0xFF;
            hdr[3] = (bytes_read >> 24) & 0xFF;
            hdr[4] = (row >>  0) & 0xFF;
            hdr[5] = (row >>  8) & 0xFF;
            hdr[6] = (row >> 16) & 0xFF;
            uart_send_buf(hdr, 7);

            /* Stream page data */
            uint32_t words = (bytes_read + 3) / 4;
            for (uint32_t i = 0; i < words; i++) {
                uint32_t w = nand_read(REG_PAGE_BUF + i * 4);
                uint8_t b[4];
                b[0] = (w >>  0) & 0xFF;
                b[1] = (w >>  8) & 0xFF;
                b[2] = (w >> 16) & 0xFF;
                b[3] = (w >> 24) & 0xFF;
                uint32_t remaining = bytes_read - i * 4;
                uint32_t send_cnt = remaining >= 4 ? 4 : remaining;
                uart_send_buf(b, send_cnt);
            }
        }

        /* Progress: send block number on a separate line every 64 blocks */
        if ((block & 0x3F) == 0) {
            uart_send_str("\r\nBLK=");
            uart_send_hex32(block);
            uart_send_str("\r\n");
        }
    }

    uart_send_str("DUMP_END\r\n");
}

/*---------------------------------------------------------------------------
 * Main
 *---------------------------------------------------------------------------*/
int main(void)
{
    uart_init();

    uart_send_str("\r\n");
    uart_send_str("========================================\r\n");
    uart_send_str("  NAND Flash Dumper for SD Card Recovery\r\n");
    uart_send_str("  Arty Z7 / Zynq XC7Z020\r\n");
    uart_send_str("========================================\r\n");

    /* Verify FPGA design is loaded */
    uint32_t ver = nand_read(REG_VERSION);
    if (ver == 0x4E414E44) {
        uart_send_str("FPGA: OK (version 'NAND')\r\n");
    } else {
        uart_send_str("FPGA: ERROR — unexpected version 0x");
        uart_send_hex32(ver);
        uart_send_str("\r\n");
        uart_send_str("Check that the bitstream is loaded.\r\n");
    }

    /* Initial NAND reset */
    uart_send_str("Resetting NAND...\r\n");
    do_reset();

    uart_send_str("Ready. Commands: R=Reset I=ID S=Status G=GetPage D=DumpAll V=Version\r\n");
    uart_send_str("> ");

    while (1) {
        uint8_t cmd = uart_recv_byte();
        uart_send_byte(cmd);  /* echo */
        uart_send_str("\r\n");

        switch (cmd) {
        case 'R': case 'r':
            do_reset();
            break;

        case 'I': case 'i':
            do_read_id();
            break;

        case 'S': case 's':
            do_read_status();
            break;

        case 'G': case 'g':
            do_read_page();
            break;

        case 'D': case 'd':
            uart_send_str("Starting full dump. Ensure host receiver is running.\r\n");
            do_dump_all();
            break;

        case 'A': case 'a': {
            /* Set address: receive 5 bytes */
            uart_send_str("Send 5 addr bytes (col_lo col_hi row0 row1 row2): ");
            uint8_t ab[5];
            for (int i = 0; i < 5; i++)
                ab[i] = uart_recv_byte();
            nand_write(REG_ADDR_COL, ab[0] | (ab[1] << 8));
            nand_write(REG_ADDR_ROW, ab[2] | (ab[3] << 8) | (ab[4] << 16));
            uart_send_str("ADDR_COL=0x");
            uart_send_hex32(nand_read(REG_ADDR_COL));
            uart_send_str(" ADDR_ROW=0x");
            uart_send_hex32(nand_read(REG_ADDR_ROW));
            uart_send_str("\r\n");
            break;
        }

        case 'C': case 'c': {
            /* Set read byte count: receive 2 bytes */
            uart_send_str("Send 2 count bytes (lo hi): ");
            uint8_t cb[2];
            cb[0] = uart_recv_byte();
            cb[1] = uart_recv_byte();
            nand_write(REG_RD_COUNT, cb[0] | (cb[1] << 8));
            uart_send_str("RD_COUNT=0x");
            uart_send_hex32(nand_read(REG_RD_COUNT));
            uart_send_str("\r\n");
            break;
        }

        case 'V': case 'v':
            uart_send_str("VERSION=0x");
            uart_send_hex32(nand_read(REG_VERSION));
            uart_send_str("\r\n");
            break;

        case '\r': case '\n':
            break;

        default:
            uart_send_str("Unknown command. R=Reset I=ID S=Status G=GetPage D=DumpAll A=SetAddr C=SetCount V=Version\r\n");
            break;
        }

        uart_send_str("> ");
    }

    return 0;
}
