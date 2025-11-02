#ifndef DMA_SDRAM_H
#define DMA_SDRAM_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>


#define MAX_DMA_SEGMENTS 4793490
#define FRAME_SIZE       56

// ===== Function declarations =====

int dma_init(void);
void print_signed16(uint8_t *buf, size_t offset, int count);
void dma_read_values(uint8_t *read_array);
volatile uint8_t* dma_get_buffer(void);
void trig_buffer_reading_blocked(void);


#endif // DMA_SDRAM_H
