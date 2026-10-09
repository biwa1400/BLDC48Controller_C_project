#ifndef DMA_SDRAM_H
#define DMA_SDRAM_H

#include <stddef.h>
#include <stdint.h>

#define FRAME_SIZE 108u
#define MAX_DMA_SEGMENTS 2485512u
#define DMA_BUFFER_PHYS 0x10000000u
#define DMA_BUFFER_SIZE 0x10000000u
#define DMA_CONTROL_LW_OFFSET 0x00020000u

#define DMA_REG_RECORD_LIMIT     0x00u
#define DMA_REG_BUFFER_BASE      0x04u
#define DMA_REG_RECORDED_COUNT   0x08u
#define DMA_REG_RECORD_CONTROL  0x0Cu
#define DMA_REG_FRAME_SEQ       0x10u
#define DMA_REG_STATUS          0x14u

#define DMA_STATUS_FRAME_VALID       0x01u
#define DMA_STATUS_RECORDING_ACTIVE  0x02u
#define DMA_STATUS_BUSY              0x04u
#define DMA_STATUS_RECORDING_DONE    0x08u

int dma_init(void);
void dma_close(void);
int dma_read_live_frame(uint8_t out[FRAME_SIZE]);
void dma_read_values(uint8_t *read_array);
volatile uint8_t *dma_get_buffer(void);
uint32_t dma_get_recorded_count(void);
uint32_t dma_get_frame_seq(void);
uint32_t dma_get_status(void);
int dma_set_record_limit(uint32_t frames);
int dma_start_recording(void);
void print_signed16(uint8_t *buf, size_t offset, int count);

#endif
