
#include "thd.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#define THD_PI 3.14159265358979323846
#define THD_MIN_FUNDAMENTAL_AMPLITUDE 1.0e-6

static int thd_finite(double value)
{
    return value == value && value <= DBL_MAX && value >= -DBL_MAX;
}

void thd_reset_channel_window(ThdAnalyzer *analyzer, uint32_t channel)
{
    ThdChannelAnalyzer *item;

    if (analyzer == NULL || channel >= THD_CHANNELS)
        return;

    item = &analyzer->channel[channel];
    memset(item->state, 0, sizeof(item->state));
    item->samples_in_window = 0u;
}

void thd_reset_window(ThdAnalyzer *analyzer)
{
    uint32_t ch;

    if (analyzer == NULL)
        return;

    for (ch = 0u; ch < THD_CHANNELS; ++ch)
        thd_reset_channel_window(analyzer, ch);
}

bool thd_init(ThdAnalyzer *analyzer, float sample_rate_hz)
{
    if (analyzer == NULL ||
        !thd_finite((double)sample_rate_hz) ||
        sample_rate_hz <= 0.0f)
        return false;

    memset(analyzer, 0, sizeof(*analyzer));
    analyzer->sample_rate_hz = sample_rate_hz;

    return true;
}

bool thd_set_channel_fundamental(ThdAnalyzer *analyzer,
                                 uint32_t channel,
                                 float fundamental_hz)
{
    ThdChannelAnalyzer *item;
    double nyquist;
    double max_frequency;
    double frequency;
    double target;
    uint32_t h;
    uint32_t count;
    uint16_t mask;

    if (analyzer == NULL || channel >= THD_CHANNELS)
        return false;

    item = &analyzer->channel[channel];

    if (item->configured &&
        item->fundamental_hz == fundamental_hz)
        return true;

    item->configured = false;
    item->fundamental_hz = 0.0f;
    item->harmonic_count = 0u;
    item->harmonic_valid_mask = 0u;
    item->target_samples = 0u;

    thd_reset_channel_window(analyzer, channel);

    if (!thd_finite((double)fundamental_hz) ||
        fundamental_hz <= 0.0f ||
        !thd_finite((double)analyzer->sample_rate_hz) ||
        analyzer->sample_rate_hz <= 0.0f)
        return false;

    nyquist = 0.5 * (double)analyzer->sample_rate_hz;
    max_frequency = (double)THD_BANDWIDTH_HZ;

    if (nyquist < max_frequency)
        max_frequency = nyquist;

    if (2.0 * (double)fundamental_hz >
            (double)THD_BANDWIDTH_HZ ||
        2.0 * (double)fundamental_hz >= nyquist)
        return false;

    count = 0u;
    mask = 0u;

    for (h = 1u; h <= THD_MAX_HARMONICS; ++h) {
        frequency = (double)h * (double)fundamental_hz;

        if (frequency > max_frequency || frequency >= nyquist)
            break;

        item->coefficient[h - 1u] =
            2.0 * cos(2.0 * THD_PI * frequency /
                      (double)analyzer->sample_rate_hz);

        mask = (uint16_t)(
            mask | (uint16_t)(1u << (h - 1u))
        );

        ++count;
    }

    if (count < 2u)
        return false;

    target = (double)THD_WINDOW_CYCLES *
             (double)analyzer->sample_rate_hz /
             (double)fundamental_hz;

    if (!thd_finite(target) ||
        target > (double)UINT32_MAX - 0.5 ||
        target < 2.0)
        return false;

    item->fundamental_hz = fundamental_hz;
    item->harmonic_count = (uint8_t)count;
    item->harmonic_valid_mask = mask;
    item->target_samples = (uint32_t)floor(target + 0.5);
    item->configured = true;

    return true;
}

bool thd_push_frame(ThdAnalyzer *analyzer,
                    const float currents[THD_CHANNELS],
                    ThdResult *result)
{
    uint32_t ch;
    uint32_t h;
    uint32_t valid_count;
    int completed;
    double sample;
    double q0;
    double thd_value;
    double a;
    double b;
    double power;
    double amplitude;
    double fundamental_power;
    double harmonic_power;
    double sum_thd;
    float max_thd;
    ThdChannelAnalyzer *item;
    ThdGoertzelState *state;

    if (analyzer == NULL || currents == NULL || result == NULL)
        return false;

    completed = 0;

    for (ch = 0u; ch < THD_CHANNELS; ++ch) {
        item = &analyzer->channel[ch];

        if (!item->configured) {
            result->valid_mask &=
                ~((uint32_t)1u << ch);
            result->fundamental_hz[ch] = 0.0f;
            result->channel_thd_percent[ch] = -1.0f;
            result->harmonic_valid_mask[ch] = 0u;

            memset(result->harmonics[ch],
                   0,
                   sizeof(result->harmonics[ch]));
            continue;
        }

        sample = (double)currents[ch];

        if (!thd_finite(sample)) {
            thd_reset_channel_window(analyzer, ch);

            result->valid_mask &=
                ~((uint32_t)1u << ch);
            result->channel_thd_percent[ch] = -1.0f;
            result->harmonic_valid_mask[ch] = 0u;
            continue;
        }

        for (h = 0u; h < item->harmonic_count; ++h) {
            state = &item->state[h];

            q0 = sample +
                 item->coefficient[h] * state->q1 -
                 state->q2;

            state->q2 = state->q1;
            state->q1 = q0;
        }

        ++item->samples_in_window;

        if (item->samples_in_window < item->target_samples)
            continue;

        completed = 1;
        fundamental_power = 0.0;
        harmonic_power = 0.0;

        result->fundamental_hz[ch] = item->fundamental_hz;
        result->harmonic_valid_mask[ch] = 0u;

        memset(result->harmonics[ch],
               0,
               sizeof(result->harmonics[ch]));

        for (h = 0u; h < item->harmonic_count; ++h) {
            state = &item->state[h];

            a = state->q1;
            b = state->q2;

            power = a * a + b * b -
                    item->coefficient[h] * a * b;

            if (!thd_finite(power))
                break;

            if (power < 0.0)
                power = 0.0;

            amplitude = 2.0 * sqrt(power) /
                        (double)item->target_samples;

            if (!thd_finite(amplitude) ||
                amplitude > (double)FLT_MAX)
                break;

            result->harmonics[ch][h] = (float)amplitude;

            result->harmonic_valid_mask[ch] |=
                (uint16_t)(1u << h);

            if (h == 0u)
                fundamental_power = power;
            else
                harmonic_power += power;
        }

        if (h == item->harmonic_count &&
            result->harmonics[ch][0] >=
                THD_MIN_FUNDAMENTAL_AMPLITUDE &&
            fundamental_power > 0.0 &&
            thd_finite(harmonic_power)) {

            thd_value = 100.0 *
                        sqrt(harmonic_power /
                             fundamental_power);

            if (thd_finite(thd_value) &&
                thd_value <= (double)FLT_MAX) {
                result->channel_thd_percent[ch] =
                    (float)thd_value;

                result->valid_mask |=
                    (uint32_t)1u << ch;
            } else {
                result->channel_thd_percent[ch] = -1.0f;
                result->valid_mask &=
                    ~((uint32_t)1u << ch);
            }
        } else {
            result->channel_thd_percent[ch] = -1.0f;
            result->valid_mask &=
                ~((uint32_t)1u << ch);
            result->harmonic_valid_mask[ch] = 0u;
        }

        thd_reset_channel_window(analyzer, ch);
    }

    if (!completed)
        return false;

    sum_thd = 0.0;
    max_thd = -1.0f;
    valid_count = 0u;

    for (ch = 0u; ch < THD_CHANNELS; ++ch) {
        if ((result->valid_mask &
             ((uint32_t)1u << ch)) != 0u) {

            sum_thd +=
                (double)result->channel_thd_percent[ch];

            if (result->channel_thd_percent[ch] > max_thd)
                max_thd = result->channel_thd_percent[ch];

            ++valid_count;
        }
    }

    result->total_thd_percent =
        valid_count > 0u ?
        (float)(sum_thd / (double)valid_count) :
        -1.0f;

    result->max_thd_percent = max_thd;
    result->sequence = ++analyzer->sequence;

    return true;
}
