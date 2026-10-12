#include "thd_frequency.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

/* At 20 kHz, alpha=0.00001 gives a DC tracking time constant of ~5 s.
 * This preserves the 0.1 Hz component much better than alpha=0.001.
 * DC removal is used only for frequency detection, not for THD samples.
 */
#define THD_FREQ_DC_ALPHA       0.00001f
#define THD_FREQ_AMP_ALPHA      0.0005f
#define THD_FREQ_TIMEOUT_CYCLES 3.0f

static int frequency_finite(float x)
{
    return x == x && x <= FLT_MAX && x >= -FLT_MAX;
}

void thd_frequency_reset(ThdFrequencyDetector *detector)
{
    float sample_rate;
    if (detector == NULL)
        return;
    sample_rate = detector->sample_rate_hz;
    memset(detector, 0, sizeof(*detector));
    detector->sample_rate_hz = sample_rate;
}

bool thd_frequency_init(ThdFrequencyDetector *detector, float sample_rate_hz)
{
    if (detector == NULL || !frequency_finite(sample_rate_hz) ||
        sample_rate_hz <= 2.0f * THD_FREQ_MAX_HZ)
        return false;
    memset(detector, 0, sizeof(*detector));
    detector->sample_rate_hz = sample_rate_hz;
    return true;
}

float thd_frequency_get(const ThdFrequencyDetector *detector)
{
    if (detector == NULL || !detector->valid)
        return 0.0f;
    return detector->frequency_hz;
}

bool thd_frequency_update(ThdFrequencyDetector *detector,
                          float current, uint32_t sample_index)
{
    float centered;
    float magnitude;
    float threshold;
    float estimate;
    float max_delta;
    uint32_t elapsed;
    uint32_t period;
    uint32_t max_period;
    uint32_t min_period;
    int rising;

    if (detector == NULL || !frequency_finite(current) ||
        !frequency_finite(detector->sample_rate_hz) ||
        detector->sample_rate_hz <= 0.0f)
        return false;

    if (!detector->have_previous) {
        detector->dc_offset = current;
        detector->previous_sample = 0.0f;
        detector->previous_index = sample_index;
        detector->have_previous = true;
        return false;
    }

    elapsed = sample_index - detector->previous_index;
    if (elapsed == 0u)
        return detector->valid;
    detector->previous_index = sample_index;

    if (elapsed != 1u) {
        thd_frequency_reset(detector);
        detector->dc_offset = current;
        detector->previous_index = sample_index;
        detector->have_previous = true;
        return false;
    }

    detector->dc_offset += THD_FREQ_DC_ALPHA *
                           (current - detector->dc_offset);
    centered = current - detector->dc_offset;
    magnitude = (float)fabs((double)centered);
    detector->amplitude += THD_FREQ_AMP_ALPHA *
                           (magnitude - detector->amplitude);

    max_period = (uint32_t)(detector->sample_rate_hz / THD_FREQ_MIN_HZ + 0.5f);
    min_period = (uint32_t)(detector->sample_rate_hz / THD_FREQ_MAX_HZ);
    if (min_period < 2u)
        min_period = 2u;

    if (detector->have_crossing) {
        elapsed = sample_index - detector->last_crossing_index;
        if (elapsed > (uint32_t)(THD_FREQ_TIMEOUT_CYCLES * (float)max_period) ||
            (detector->valid && detector->frequency_hz > 0.0f &&
             (float)elapsed > THD_FREQ_TIMEOUT_CYCLES *
             detector->sample_rate_hz / detector->frequency_hz)) {
            detector->valid = false;
            detector->have_crossing = false;
            detector->valid_cycles = 0u;
            detector->frequency_hz = 0.0f;
        }
    }

    if (detector->amplitude < THD_FREQ_MIN_AMPLITUDE) {
        detector->valid = false;
        detector->have_crossing = false;
        detector->valid_cycles = 0u;
        detector->frequency_hz = 0.0f;
        detector->crossing_state = 0u;
        detector->previous_sample = centered;
        return false;
    }

    threshold = THD_FREQ_HYSTERESIS * detector->amplitude;
    if (threshold < 0.5f)
        threshold = 0.5f;

    rising = 0;
    if (detector->crossing_state == 0u) {
        if (centered <= -threshold)
            detector->crossing_state = 1u;
    } else if (centered >= threshold) {
        rising = 1;
        detector->crossing_state = 0u;
    }

    if (rising) {
        if (detector->have_crossing) {
            period = sample_index - detector->last_crossing_index;
            if (period >= min_period && period <= max_period) {
                estimate = detector->sample_rate_hz / (float)period;
                if (detector->valid_cycles == 0u) {
                    detector->frequency_hz = estimate;
                    detector->valid_cycles = 1u;
                } else {
                    max_delta = 0.25f * detector->frequency_hz;
                    if ((float)fabs((double)(estimate - detector->frequency_hz)) <=
                        max_delta) {
                        detector->frequency_hz += THD_FREQ_SMOOTH_ALPHA *
                            (estimate - detector->frequency_hz);
                        if (detector->valid_cycles < THD_FREQ_VALID_CYCLES)
                            ++detector->valid_cycles;
                    } else {
                        detector->frequency_hz = estimate;
                        detector->valid_cycles = 1u;
                        detector->valid = false;
                    }
                }
                if (detector->valid_cycles >= THD_FREQ_VALID_CYCLES)
                    detector->valid = true;
            } else {
                detector->valid_cycles = 0u;
                detector->valid = false;
                detector->frequency_hz = 0.0f;
            }
        }
        detector->last_crossing_index = sample_index;
        detector->have_crossing = true;
    }

    detector->previous_sample = centered;
    return detector->valid;
}
