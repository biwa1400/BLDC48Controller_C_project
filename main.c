
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "dma_sdram.h"
#include "data_writer.h"
#include "data_socket.h"
#include "control_socket.h"
#include "control_motor.h"
#include "sram_worker.h"

uint8_t handle_command(uint8_t cmd) {
    int pid;

    printf("Executing command: 0x%02X\n", (unsigned)cmd);
    switch (cmd) {
        case 0x01:
            printf("Command RUN\n");
            run();
            return 0x00;

        case 0x02:
            printf("Command STOP\n");
            stop();
            return 0x00;

        case 0x03:
            printf("Command SAMPLE\n");
            pid = start_writer_process();
            if (pid <= 0) {
                fprintf(stderr, "SAMPLE: failed to start writer process\n");
                return 0xFF;
            }
            printf("SAMPLE: writer process started (PID=%d)\n", pid);
            return 0x00;

        default:
            fprintf(stderr, "Unknown command: 0x%02X\n", (unsigned)cmd);
            return 0xFF;
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    if (dma_init() != 0) {
        fprintf(stderr, "Fatal: DMA initialization failed\n");
        return EXIT_FAILURE;
    }
    if (motor_ctrl_init() != 0) {
        fprintf(stderr, "Fatal: motor control initialization failed\n");
        dma_close();
        return EXIT_FAILURE;
    }
    if (sram_worker_init() != 0) {
        fprintf(stderr, "Fatal: SRAM worker initialization failed\n");
        dma_close();
        return EXIT_FAILURE;
    }

    register_lut_load_callback(sram_worker_submit);

    start_data_server(8888);
    start_control_server(8889, handle_command);

    for (;;) {
        sleep(10);
    }

    return EXIT_SUCCESS;
}
