#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "dma_sdram.h"
#include "data_writer.h"
#include "data_socket.h"
#include "control_socket.h"
#include "control_motor.h"
#include "mosfet_override.h"
#include "sram_worker.h"
#include "thd_worker.h"
#include "thd_socket.h"

#define THD_SAMPLE_RATE_HZ 20000.0f

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

    /* MOSFET override: fixed Platform Designer lightweight-bridge offset.
     * Same address convention as DMA_CONTROL_LW_OFFSET.
     * The driver verifies VERSION (0x4D4F0001) before enabling access.
     * Initialization does not enable override or change any output.
     */
    if (mosfet_override_init(0x00030000u) != 0) {
        fprintf(stderr, "[MOSFET] FPGA register initialization failed "
                        "at LW offset 0x30000: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    printf("[MOSFET] FPGA override interface ready at LW offset 0x30000\n");

    register_lut_load_callback(sram_worker_submit);

    start_data_server(8888);
    start_control_server(8889, handle_command);

    if (thd_worker_start(THD_SAMPLE_RATE_HZ) != 0) {
        fprintf(stderr, "Fatal: THD worker initialization failed\n");
        return EXIT_FAILURE;
    }
    printf("[THD] Worker started, sample rate: %.0f Hz\n",
           THD_SAMPLE_RATE_HZ);

    if (thd_socket_start() != 0) {
        fprintf(stderr, "Fatal: THD TCP server initialization failed\n");
        thd_worker_stop();
        return EXIT_FAILURE;
    }
    printf("[THD] TCP server startup requested, port: 10001\n");

    for (;;) {
        sleep(10);
    }

    return EXIT_SUCCESS;
}
