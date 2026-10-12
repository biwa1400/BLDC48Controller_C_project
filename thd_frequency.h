#ifndef THD_FREQUENCY_H
#define THD_FREQUENCY_H

#include <stdint.h>
#include "thd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define THD_FREQ_MIN_HZ          0.1f
#define THD_FREQ_MAX_HZ          1000.0f
#define THD_FREQ_SMOOTH_ALPHA    0.2f
#define THD_FREQ_HYSTERESIS      0.1f
#define THD_FREQ_MIN_AMPLITUDE   5.0f
#define THD_FREQ_VALID_CYCLES    3u

typedef struct {
    float sample_rate_hz;
    float dc_offset;
    float amplitude;
    float frequency_hz;
    float previous_sample;
    uint32_t previous_index;
    uint32_t last_crossing_index;
    uint32_t valid_cycles;
    uint8_t crossing_state;
    bool have_previous;
    bool have_crossing;
    bool valid;
} ThdFrequencyDetector;

/* Initialize one independent frequency detector. */
bool thd_frequency_init(
    ThdFrequencyDetector *detector,
    float sample_rate_hz
);

/* Process one current sample with its actual sample index.
 * Returns true when a valid frequency estimate is available.
 */
bool thd_frequency_update(
    ThdFrequencyDetector *detector,
    float current,
    uint32_t sample_index
);

/* Return the latest frequency, or 0 if invalid. */
float thd_frequency_get(
    const ThdFrequencyDetector *detector
);

/* Reset one detector without changing its sample rate. */
void thd_frequency_reset(
    ThdFrequencyDetector *detector
);

#ifdef __cplusplus
}
#endif

#endif /* THD_FREQUENCY_H */
