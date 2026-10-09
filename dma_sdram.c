#include "dma_sdram.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define HPS_LW_BRIDGE_PHYS 0xFF200000u
#define HPS_LW_BRIDGE_SPAN 0x00040000u

static volatile uint32_t *dma_ctrl_regs;
static volatile uint8_t *dma_buffer;
static void *control_mapping;
static void *buffer_mapping;
static int mem_fd = -1;

static uint32_t reg_read(uint32_t byte_offset) {
    return dma_ctrl_regs[byte_offset / sizeof(uint32_t)];
}

static void reg_write(uint32_t byte_offset, uint32_t value) {
    dma_ctrl_regs[byte_offset / sizeof(uint32_t)] = value;
}

void dma_close(void) {
    if (buffer_mapping) {
        munmap(buffer_mapping, DMA_BUFFER_SIZE);
        buffer_mapping = NULL;
    }
    if (control_mapping) {
        munmap(control_mapping, HPS_LW_BRIDGE_SPAN);
        control_mapping = NULL;
    }
    if (mem_fd >= 0) {
        close(mem_fd);
        mem_fd = -1;
    }
    dma_ctrl_regs = NULL;
    dma_buffer = NULL;
}

int dma_init(void) {
    dma_close();
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        perror("DMA: open /dev/mem");
        return -1;
    }
    control_mapping = mmap(NULL, HPS_LW_BRIDGE_SPAN, PROT_READ | PROT_WRITE,
                           MAP_SHARED, mem_fd, HPS_LW_BRIDGE_PHYS);
    if (control_mapping == MAP_FAILED) {
        control_mapping = NULL;
        perror("DMA: mmap lightweight bridge");
        dma_close();
        return -1;
    }
    dma_ctrl_regs = (volatile uint32_t *)((uint8_t *)control_mapping + DMA_CONTROL_LW_OFFSET);
    buffer_mapping = mmap(NULL, DMA_BUFFER_SIZE, PROT_READ | PROT_WRITE,
                          MAP_SHARED, mem_fd, DMA_BUFFER_PHYS);
    if (buffer_mapping == MAP_FAILED) {
        buffer_mapping = NULL;
        perror("DMA: mmap SDRAM buffer");
        dma_close();
        return -1;
    }
    dma_buffer = (volatile uint8_t *)buffer_mapping;
    printf("DMA mapped: control=0x%08X, buffer=0x%08X, frame=%u bytes\n",
           HPS_LW_BRIDGE_PHYS + DMA_CONTROL_LW_OFFSET,
           DMA_BUFFER_PHYS, (unsigned)FRAME_SIZE);
    printf("[DMA] Live frame read: %u aligned 32-bit words (no sequence check)\n",
           (unsigned)(FRAME_SIZE / sizeof(uint32_t)));
    /* Do not start recording or overwrite register settings during initialization. */
    return 0;
}

uint32_t dma_get_recorded_count(void) {
    return dma_ctrl_regs ? reg_read(DMA_REG_RECORDED_COUNT) : 0;
}

uint32_t dma_get_frame_seq(void) {
    return dma_ctrl_regs ? reg_read(DMA_REG_FRAME_SEQ) : 0;
}

uint32_t dma_get_status(void) {
    return dma_ctrl_regs ? reg_read(DMA_REG_STATUS) : 0;
}

int dma_set_record_limit(uint32_t frames) {
    if (!dma_ctrl_regs || frames == 0 || frames > MAX_DMA_SEGMENTS) {
        errno = EINVAL;
        return -1;
    }
    reg_write(DMA_REG_RECORD_LIMIT, frames);
    return 0;
}

int dma_start_recording(void) {
    if (!dma_ctrl_regs) { errno = ENODEV; return -1; }
    if ((reg_read(DMA_REG_STATUS) & DMA_STATUS_RECORDING_ACTIVE) != 0) {
        errno = EBUSY;
        return -1;
    }
    reg_write(DMA_REG_RECORD_CONTROL, 1u);
    return 0;
}

int dma_read_live_frame(uint8_t out[FRAME_SIZE]) {
    const volatile uint32_t *words;
    uint32_t value;
    size_t i;
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    if (!dma_buffer) {
        errno = ENODEV;
        return -1;
    }
    words = (const volatile uint32_t *)(const void *)dma_buffer;
    for (i = 0; i < FRAME_SIZE / sizeof(uint32_t); ++i) {
        value = words[i];
        memcpy(out + i * sizeof(value), &value, sizeof(value));
    }
    return 0;
}

void dma_read_values(uint8_t *read_array) {
    if (read_array && dma_read_live_frame(read_array) != 0)
        memset(read_array, 0, FRAME_SIZE);
}

volatile uint8_t *dma_get_buffer(void) {
    return dma_buffer;
}

void print_signed16(uint8_t *data, size_t offset, int count) {
    int i;
    if (!data || count < 0) return;
    for (i = 0; i < count; ++i) {
        int16_t value;
        memcpy(&value, data + offset + (size_t)i * 2u, sizeof(value));
        printf("%6d (0x%04X)  ", value, (uint16_t)value);
        if ((i + 1) % 4 == 0) putchar('\n');
    }
    putchar('\n');
}
