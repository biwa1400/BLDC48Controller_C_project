
#ifndef THD_H
#define THD_H

#include <stdint.h>

/* C89-compatible bool */
#if !defined(__cplusplus)
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
#include <stdbool.h>
#else
#ifndef __bool_true_false_are_defined
typedef unsigned char bool;
#define true 1
#define false 0
#define __bool_true_false_are_defined 1
#endif
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define THD_CHANNELS        22u
#define THD_MAX_HARMONICS   10u
#define THD_BANDWIDTH_HZ    1000.0f
#define THD_WINDOW_CYCLES   5u

/* Per-harmonic Goertzel state.
 * Double precision is required for low-frequency analysis.
 */
typedef struct {
    double q1;
    double q2;
} ThdGoertzelState;

/* One completed measurement snapshot */
typedef struct {
    uint32_t sequence;

    /* Independent fundamental frequency for each channel */
    float fundamental_hz[THD_CHANNELS];

    /* THD percentage for each channel */
    float channel_thd_percent[THD_CHANNELS];

    /* H1-H10 amplitudes for each channel */
    float harmonics[THD_CHANNELS][THD_MAX_HARMONICS];

    /* Valid THD channels: bit 0 = I1, bit 21 = I22 */
    uint32_t valid_mask;

    /* Valid harmonics: bit 0 = H1, bit 9 = H10 */
    uint16_t harmonic_valid_mask[THD_CHANNELS];

    /* Summary of valid channels */
    float total_thd_percent;
    float max_thd_percent;
} ThdResult;

/* Independent analyzer state for one current channel */
typedef struct {
    float fundamental_hz;
    double coefficient[THD_MAX_HARMONICS];
    ThdGoertzelState state[THD_MAX_HARMONICS];

    uint32_t samples_in_window;
    uint32_t target_samples;

    uint16_t harmonic_valid_mask;
    uint8_t harmonic_count;
    bool configured;
} ThdChannelAnalyzer;

/* Analyzer for all 22 channels */
typedef struct {
    float sample_rate_hz;

    ThdChannelAnalyzer channel[THD_CHANNELS];

    uint32_t sequence;
} ThdAnalyzer;

/* Initialize all channel analyzers */
bool thd_init(ThdAnalyzer *analyzer, float sample_rate_hz);

/* Set fundamental frequency for one channel */
bool thd_set_channel_fundamental(
    ThdAnalyzer *analyzer,
    uint32_t channel,
    float fundamental_hz
);

/* Push one 22-channel current sample frame */
bool thd_push_frame(
    ThdAnalyzer *analyzer,
    const float currents[THD_CHANNELS],
    ThdResult *result
);

/* Reset measurement window for one channel */
void thd_reset_channel_window(
    ThdAnalyzer *analyzer,
    uint32_t channel
);

/* Reset all channel measurement windows */
void thd_reset_window(ThdAnalyzer *analyzer);

#ifdef __cplusplus
}
#endif

#endif /* THD_H */
