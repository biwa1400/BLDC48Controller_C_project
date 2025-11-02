#include "dma_sdram.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>

#define soc_cv_av
#include "hwlib.h"
#include "socal/socal.h"
#include "socal/hps.h"
#include "hps_0.h"



#define HW_REGS_BASE (ALT_STM_OFST)
#define HW_REGS_SPAN (0x04000000)
#define HW_REGS_MASK (HW_REGS_SPAN - 1)

#define REG_SEGMENT_MAX_OFFSET   0
#define REG_SEGMENT_BASE_ADDRESS 1
#define REG_SEGMENT_OFFSET       2
#define REG_SEGMENT_CONTROL      3

#define BUFFER_PHYS 0x10000000  // 物理起始地址
#define BUFFER_SIZE 0x10000000  // 256 MB

static volatile unsigned long *dma_ctrl_regs = NULL; // DMA 控制寄存器虚拟地址
static volatile uint8_t *buf = NULL;

// 初始化 DMA 控制器
int dma_init(void) {
    int fd;
    void *periph_virtual_base;

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd == -1) {
        perror("open /dev/mem");
        return -1;
    }

    periph_virtual_base = mmap(NULL, HW_REGS_SPAN, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, HW_REGS_BASE);
    if (periph_virtual_base == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return -1;
    }

    dma_ctrl_regs = (volatile unsigned long *)(periph_virtual_base +
                    (ALT_LWFPGASLVS_OFST & HW_REGS_MASK));

    dma_ctrl_regs[REG_SEGMENT_MAX_OFFSET]   = MAX_DMA_SEGMENTS;
    dma_ctrl_regs[REG_SEGMENT_BASE_ADDRESS] = BUFFER_PHYS;

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem for buffer");
        return -1;
    }

    buf = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, BUFFER_PHYS);
    if (buf == MAP_FAILED) {
        perror("mmap buffer");
        return -1;
    }

    return 0;
}

// 打印 16-bit 补码并映射到 ±5V
void print_signed16(uint8_t *buf, size_t offset, int count) {
    int16_t *buf16 = (int16_t *)buf;
    size_t start_index = offset / 2;
    int i=0;

    printf("Print %d x 16-bit signed values from offset %zu:\n", count, offset);
    for (i = 0; i < count; i++) {
        int16_t val = buf16[start_index + i];
        uint16_t raw = (uint16_t)val;
        double voltage = (double)val * 5.0 / 4096.0;
        printf("%6d (0x%04X) -> %+6.3f V  ", val, raw, voltage);
        if ((i + 1) % 4 == 0) printf("\n");
    }
    printf("\n");
}

// 从 SDRAM 读取数据到传入数组
void dma_read_values(uint8_t *read_array) {
    if (!buf || !dma_ctrl_regs || !read_array) return;

    uint32_t offset = dma_ctrl_regs[REG_SEGMENT_OFFSET];
    //printf("offset!!!!!!! %d", offset);
    // 计算偏移地址
    uint8_t *src = (uint8_t *)(buf + offset * FRAME_SIZE); // 每段假设 52 字节
    memcpy(read_array, src, FRAME_SIZE);
}

// 返回 SDRAM buffer 的虚拟地址
volatile uint8_t* dma_get_buffer(void) {
    return buf;
}

void trig_buffer_reading_blocked(void) {
	dma_ctrl_regs[REG_SEGMENT_CONTROL]   = 1;
	while(dma_ctrl_regs[REG_SEGMENT_CONTROL]==0);
	return;
}

