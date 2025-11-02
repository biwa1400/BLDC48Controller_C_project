// sdram_dma_test.c
// User-space test program: SDRAM DMA Control Interface
// ============================================================
// Register mapping:
//   0x0 SEGMENT_MAX_OFFSET   (RW) Max number of transfer segments
//   0x1 SEGMENT_BASE_ADDRESS (RW) SDRAM base address, writing triggers init
//   0x2 SEGMENT_OFFSET       (RO) Current DMA segment offset
//   0x3 Reserved             (RO)
// ============================================================

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <sys/ioctl.h>

// HPS platform macro
#define soc_cv_av

// HPS peripheral headers
#include "hwlib.h"
#include "socal/socal.h"
#include "socal/hps.h"
#include "hps_0.h"

#include "dma_sdram.h"
#include "data_writer.h"
#include "data_socket.h"
#include "control_socket.h"
#include "control_motor.h"


uint8_t handle_command(uint8_t cmd) {
    printf("Executing command: 0x%02X\n", cmd);
    switch (cmd) {
        case 0x01:
            printf("Command RUN\n");
            run();
            return 0x00; // success
        case 0x02:
            printf("Command STOP\n");
            stop();
            return 0x00;
        case 0x03:
            printf("Command SAMPLE\n");
            start_writer_process();
            return 0xFF;
    }
    return 0xFF;
}

int main(int argc, char **argv) {
	dma_init();
	motor_ctrl_init();
	//start_writer_process();
	start_data_server(8888);
	start_control_server(8889, handle_command);

    while (1) {
        sleep(10);  // ×èÈûÖ÷Ïß³Ì
    }

    return 0;
}


