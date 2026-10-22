#ifndef MOSFET_OVERRIDE_H
#define MOSFET_OVERRIDE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MOSFET manual override: 22 high-side and 22 low-side gate outputs.
 *
 * Channel numbering:
 *   0..21  = low-side  pwm_lo[0..21]
 *   22..43 = high-side pwm_hi[0..21]
 *
 * These functions access the FPGA Avalon-MM override peripheral.
 * Register base address is supplied at initialization and MUST match
 * the final Platform Designer address assignment.
 *
 * Manual mode bypasses normal PWM deadtime/interlock. Use only with
 * the power stage isolated until gate outputs are independently verified.
 */

#define MOSFET_OVERRIDE_CHANNEL_COUNT 44u

typedef struct {
    uint8_t  enabled;       /* 0 = normal PWM, 1 = manual override */
    uint32_t outputs_lo;    /* low-side outputs, bits [21:0] */
    uint32_t outputs_hi;    /* high-side outputs, bits [21:0] */
} mosfet_override_state_t;

/* Initialize register mapping; avalon_base is the LW bridge byte offset. */
int mosfet_override_init(uint32_t avalon_base);

/* Read back current FPGA register state. */
int mosfet_override_get_state(mosfet_override_state_t *state);

/* Select normal (0) or manual (1) output mode. */
int mosfet_override_set_mode(uint8_t enable);

/* Set one output bit. Channel must be 0..43; level must be 0 or 1. */
int mosfet_override_set_channel(uint8_t channel, uint8_t level);

/* Release mapped resources; does not change FPGA output state. */
void mosfet_override_close(void);

#ifdef __cplusplus
}
#endif

#endif /* MOSFET_OVERRIDE_H */
