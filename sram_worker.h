#ifndef SRAM_WORKER_H
#define SRAM_WORKER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SRAM_WORKER_IDLE = 0,
    SRAM_WORKER_LOADING,
    SRAM_WORKER_SUCCESS,
    SRAM_WORKER_ERROR
} sram_worker_state_t;

typedef struct {
    sram_worker_state_t state;
    uint32_t progress;
    uint32_t total;
    uint32_t error_count;
    uint32_t active_bank;
    uint32_t target_bank;
} sram_worker_status_t;

int sram_worker_init(void);
void sram_worker_shutdown(void);
uint8_t sram_worker_submit(const char *filename);
int sram_worker_get_status(sram_worker_status_t *status);

#ifdef __cplusplus
}
#endif

#endif // SRAM_WORKER_H
