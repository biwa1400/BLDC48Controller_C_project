#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>

#define soc_cv_av
#include "hwlib.h"
#include "socal/socal.h"
#include "socal/hps.h"
#include "hps_0.h"

// ================= 硬件寄存器定义 =================
#define HW_REGS_BASE       (ALT_STM_OFST)
#define HW_REGS_SPAN       (0x04000000)
#define HW_REGS_MASK       (HW_REGS_SPAN - 1)

#define CONTROL_REG_OFFSET 0x10  // 控制寄存器块偏移
#define REG_RUN_OFFSET     0   // run 寄存器在块内偏移

// ================= 全局变量 =================
static volatile uint32_t *ctrl_regs = NULL;

// ================= 公共 API =================
int motor_ctrl_init(void) {
    int fd;
    void *periph_virtual_base;

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
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

    // 直接把 ctrl_regs 指向寄存器块基地址 + CONTROL_REG_OFFSET + REG_RUN_OFFSET
    ctrl_regs = (volatile uint32_t *)(periph_virtual_base +
                                      (ALT_LWFPGASLVS_OFST & HW_REGS_MASK) +
                                      CONTROL_REG_OFFSET);
    return 0;
}


void run(void) {
    if (!ctrl_regs) return;
    ctrl_regs[REG_RUN_OFFSET] = 1;
}

void stop(void) {
    if (!ctrl_regs) return;
    ctrl_regs[REG_RUN_OFFSET] = 0;
}

