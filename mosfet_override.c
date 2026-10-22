// ============================================================================
// mosfet_override.c - ARM/Linux access to FPGA MOSFET manual override
//
// Avalon register byte offsets:
//   0x00 CTRL, 0x04 OUTPUT_LO[31:0], 0x08 OUTPUT_HI[43:32],
//   0x0C SET_LO, 0x10 CLEAR_LO, 0x14 SET_HI, 0x18 CLEAR_HI,
//   0x1C VERSION (0x4D4F0001).
//
// Logical channels: 0..21 = pwm_lo[0..21], 22..43 = pwm_hi[0..21].
// These logical channels are bits 0..43 of the FPGA's output register.
// Manual mode has no deadtime or shoot-through interlock.
// ============================================================================
#include "mosfet_override.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#define LW_BRIDGE_PHYS    UINT32_C(0xFF200000)
#define LW_BRIDGE_SPAN    UINT32_C(0x00040000)
#define PHASE_MASK        UINT32_C(0x003FFFFF)
#define OUTPUT_HIGH_MASK UINT32_C(0x00000FFF)
#define FPGA_VERSION     UINT32_C(0x4D4F0001)

#define REG_MODE         0x00u
#define REG_LOW          0x04u
#define REG_HIGH         0x08u
#define REG_SET_LOW      0x0Cu
#define REG_CLEAR_LOW    0x10u
#define REG_SET_HIGH     0x14u
#define REG_CLEAR_HIGH   0x18u
#define REG_VERSION      0x1Cu

static int mem_fd = -1;
static void *mapped_base = NULL;
static volatile uint32_t *regs = NULL;

static uint32_t read_reg(unsigned offset)
{
    return regs[offset / sizeof(uint32_t)];
}

static void write_reg(unsigned offset, uint32_t value)
{
    regs[offset / sizeof(uint32_t)] = value;
}

void mosfet_override_close(void)
{
    regs = NULL;
    if (mapped_base != NULL) {
        munmap(mapped_base, LW_BRIDGE_SPAN);
        mapped_base = NULL;
    }
    if (mem_fd >= 0) {
        close(mem_fd);
        mem_fd = -1;
    }
}

int mosfet_override_init(uint32_t avalon_base)
{
    mosfet_override_close();
    if ((avalon_base & 3u) != 0u ||
        avalon_base > LW_BRIDGE_SPAN - 0x20u) {
        errno = EINVAL;
        return -1;
    }

    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0)
        return -1;

    mapped_base = mmap(NULL, LW_BRIDGE_SPAN, PROT_READ | PROT_WRITE,
                       MAP_SHARED, mem_fd, (off_t)LW_BRIDGE_PHYS);
    if (mapped_base == MAP_FAILED) {
        int saved_errno = errno;
        mapped_base = NULL;
        mosfet_override_close();
        errno = saved_errno;
        return -1;
    }

    regs = (volatile uint32_t *)((volatile uint8_t *)mapped_base + avalon_base);

    // Never write to an unverified peripheral address.
    if (read_reg(REG_VERSION) != FPGA_VERSION) {
        mosfet_override_close();
        errno = ENODEV;
        return -1;
    }

    // Initialization does not change mode or output values.
    return 0;
}

int mosfet_override_get_state(mosfet_override_state_t *state)
{
    if (state == NULL) { errno = EINVAL; return -1; }
    if (regs == NULL) { errno = ENODEV; return -1; }

    const uint32_t bits_0_31 = read_reg(REG_LOW);
    const uint32_t bits_32_43 = read_reg(REG_HIGH) & OUTPUT_HIGH_MASK;

    state->enabled = (uint8_t)(read_reg(REG_MODE) & 1u);
    state->outputs_lo = bits_0_31 & PHASE_MASK;
    state->outputs_hi = ((bits_0_31 >> 22) | (bits_32_43 << 10)) & PHASE_MASK;
    return 0;
}

int mosfet_override_set_mode(uint8_t enable)
{
    if (enable > 1u) { errno = EINVAL; return -1; }
    if (regs == NULL) { errno = ENODEV; return -1; }
    write_reg(REG_MODE, (uint32_t)enable);
    return 0;
}

int mosfet_override_set_channel(uint8_t channel, uint8_t level)
{
    if (channel >= MOSFET_OVERRIDE_CHANNEL_COUNT || level > 1u) {
        errno = EINVAL;
        return -1;
    }
    if (regs == NULL) { errno = ENODEV; return -1; }

    // FPGA bit numbering is continuous across all 44 outputs.
    const unsigned bit = (unsigned)channel;
    const unsigned reg_bit = bit < 32u ? bit : bit - 32u;
    const uint32_t mask = UINT32_C(1) << reg_bit;
    const unsigned offset = bit < 32u
        ? (level ? REG_SET_LOW : REG_CLEAR_LOW)
        : (level ? REG_SET_HIGH : REG_CLEAR_HIGH);

    // Atomic FPGA set/clear registers avoid software read-modify-write races.
    write_reg(offset, mask);
    return 0;
}
