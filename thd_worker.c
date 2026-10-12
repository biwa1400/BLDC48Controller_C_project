#include "thd_worker.h"
#include "thd_frequency.h"
#include "dma_sdram.h"

#include <float.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>

#define THD_CURRENT_OFFSET 0u
#define THD_FREQUENCY_CHANGE_FRACTION 0.01f
#define THD_FREQUENCY_CHANGE_MIN_HZ 0.5f
#define NS_PER_SECOND 1000000000LL

static pthread_t worker_thread;
static pthread_mutex_t worker_lock = PTHREAD_MUTEX_INITIALIZER;
static int worker_running;
static int worker_stop_requested;
static ThdResult published_result;
static int result_available;
static float configured_sample_rate;

/* Direct Linux syscall avoids newer glibc clock symbol versions on ARM. */
static int read_monotonic_time(struct timespec *t)
{
    return (int)syscall(SYS_clock_gettime, CLOCK_MONOTONIC, t);
}

static int64_t timestamp_ns(const struct timespec *t)
{
    return (int64_t)t->tv_sec * NS_PER_SECOND + (int64_t)t->tv_nsec;
}

static void decode_currents(const uint8_t frame[FRAME_SIZE],
                            float currents[THD_CHANNELS])
{
    unsigned int ch;
    unsigned int offset;
    uint16_t bits;
    for (ch = 0u; ch < THD_CHANNELS; ++ch) {
        offset = THD_CURRENT_OFFSET + ch * 2u;
        bits = (uint16_t)frame[offset] |
               (uint16_t)((uint16_t)frame[offset + 1u] << 8);
        currents[ch] = (float)(int16_t)bits;
    }
}

static void invalidate_channel(ThdResult *result, unsigned int ch)
{
    result->fundamental_hz[ch] = 0.0f;
    result->channel_thd_percent[ch] = -1.0f;
    result->harmonic_valid_mask[ch] = 0u;
    result->valid_mask &= ~((uint32_t)1u << ch);
    memset(result->harmonics[ch], 0, sizeof(result->harmonics[ch]));
}

static void invalidate_thd_only(ThdResult *result, unsigned int ch)
{
    result->channel_thd_percent[ch] = -1.0f;
    result->harmonic_valid_mask[ch] = 0u;
    result->valid_mask &= ~((uint32_t)1u << ch);
    memset(result->harmonics[ch], 0, sizeof(result->harmonics[ch]));
}

static void refresh_summary(ThdResult *result)
{
    unsigned int ch;
    unsigned int count;
    double sum;
    float maximum;
    count = 0u;
    sum = 0.0;
    maximum = -1.0f;
    for (ch = 0u; ch < THD_CHANNELS; ++ch) {
        if ((result->valid_mask & ((uint32_t)1u << ch)) != 0u) {
            sum += (double)result->channel_thd_percent[ch];
            if (result->channel_thd_percent[ch] > maximum)
                maximum = result->channel_thd_percent[ch];
            ++count;
        }
    }
    result->total_thd_percent = count ? (float)(sum / count) : -1.0f;
    result->max_thd_percent = maximum;
}

static void publish_result(const ThdResult *result)
{
    pthread_mutex_lock(&worker_lock);
    published_result = *result;
    result_available = 1;
    pthread_mutex_unlock(&worker_lock);
}

static void *worker_main(void *unused)
{
    ThdAnalyzer analyzer;
    ThdFrequencyDetector detector[THD_CHANNELS];
    ThdResult current_result;
    uint8_t frame[FRAME_SIZE];
    float currents[THD_CHANNELS];
    float frequency;
    float difference;
    float tolerance;
    uint32_t sample_id;
    uint32_t ch;
    int stop;
    int changed;
    int complete;
    int frequency_valid;
    int64_t period_ns;
    int64_t deadline_ns;
    int64_t now_ns;
    struct timespec now;
    (void)unused;

    if (!thd_init(&analyzer, configured_sample_rate)) {
        fprintf(stderr, "[THD] Invalid FIR output sample rate\n");
        return NULL;
    }
    for (ch = 0u; ch < THD_CHANNELS; ++ch) {
        if (!thd_frequency_init(&detector[ch], configured_sample_rate)) {
            fprintf(stderr, "[THD] Frequency detector initialization failed\n");
            return NULL;
        }
    }
    memset(&current_result, 0, sizeof(current_result));
    for (ch = 0u; ch < THD_CHANNELS; ++ch)
        invalidate_channel(&current_result, ch);
    refresh_summary(&current_result);
    publish_result(&current_result);

    period_ns = (int64_t)(1000000000.0 / (double)configured_sample_rate + 0.5);
    if (period_ns < 1)
        period_ns = 1;
    if (read_monotonic_time(&now) != 0) {
        fprintf(stderr, "[THD] CLOCK_MONOTONIC unavailable\n");
        return NULL;
    }
    deadline_ns = timestamp_ns(&now);
    sample_id = 0u;
    for (;;) {
        pthread_mutex_lock(&worker_lock);
        stop = worker_stop_requested;
        pthread_mutex_unlock(&worker_lock);
        if (stop)
            break;

        /* ARM sample index, deliberately independent of the 500 kHz DMA sequence. */
        if (dma_read_live_frame(frame) == 0) {
            decode_currents(frame, currents);
            ++sample_id;
            changed = 0;
            for (ch = 0u; ch < THD_CHANNELS; ++ch) {
                frequency_valid = thd_frequency_update(&detector[ch],
                                                       currents[ch], sample_id);
                frequency = frequency_valid ? thd_frequency_get(&detector[ch]) : 0.0f;
                if (!frequency_valid || frequency <= 0.0f) {
                    if (analyzer.channel[ch].configured ||
                        current_result.fundamental_hz[ch] != 0.0f ||
                        (current_result.valid_mask & ((uint32_t)1u << ch)) != 0u) {
                        thd_set_channel_fundamental(&analyzer, ch, 0.0f);
                        invalidate_channel(&current_result, ch);
                        changed = 1;
                    }
                    continue;
                }
                if (!analyzer.channel[ch].configured) {
                    if (thd_set_channel_fundamental(&analyzer, ch, frequency)) {
                        current_result.fundamental_hz[ch] = frequency;
                        changed = 1;
                    } else if (current_result.fundamental_hz[ch] != 0.0f) {
                        invalidate_channel(&current_result, ch);
                        changed = 1;
                    }
                } else if (analyzer.channel[ch].samples_in_window == 0u) {
                    difference = frequency - analyzer.channel[ch].fundamental_hz;
                    if (difference < 0.0f)
                        difference = -difference;
                    tolerance = analyzer.channel[ch].fundamental_hz *
                                THD_FREQUENCY_CHANGE_FRACTION;
                    if (tolerance < THD_FREQUENCY_CHANGE_MIN_HZ)
                        tolerance = THD_FREQUENCY_CHANGE_MIN_HZ;
                    if (difference > tolerance) {
                        if (thd_set_channel_fundamental(&analyzer, ch, frequency)) {
                            invalidate_thd_only(&current_result, ch);
                            current_result.fundamental_hz[ch] = frequency;
                        } else {
                            invalidate_channel(&current_result, ch);
                        }
                        changed = 1;
                    }
                }
            }
            complete = thd_push_frame(&analyzer, currents, &current_result);
            if (complete)
                changed = 1;
            if (changed) {
                refresh_summary(&current_result);
                publish_result(&current_result);
            }
        }

        if (read_monotonic_time(&now) == 0) {
            now_ns = timestamp_ns(&now);
            deadline_ns += period_ns;
            /* If the thread ran late, do not burst-read old or duplicate samples. */
            if (deadline_ns <= now_ns)
                deadline_ns = now_ns + period_ns;
            /* Busy-wait to avoid nanosleep wake-up latency at 20 kHz.
               This consumes CPU and cannot prevent Linux scheduler jitter. */
            for (;;) {
                if (read_monotonic_time(&now) != 0)
                    break;
                if (timestamp_ns(&now) >= deadline_ns)
                    break;
            }
        }
    }
    return NULL;
}

int thd_worker_start(float sample_rate_hz)
{
    int err;
    if (!(sample_rate_hz == sample_rate_hz &&
          sample_rate_hz <= FLT_MAX && sample_rate_hz >= -FLT_MAX) ||
        sample_rate_hz <= 2.0f * THD_BANDWIDTH_HZ)
        return -1;
    pthread_mutex_lock(&worker_lock);
    if (worker_running) {
        pthread_mutex_unlock(&worker_lock);
        return -1;
    }
    worker_stop_requested = 0;
    configured_sample_rate = sample_rate_hz;
    result_available = 0;
    err = pthread_create(&worker_thread, NULL, worker_main, NULL);
    if (!err)
        worker_running = 1;
    pthread_mutex_unlock(&worker_lock);
    return err ? -1 : 0;
}

bool thd_worker_get_result(ThdResult *out)
{
    int available;
    if (out == NULL)
        return false;
    pthread_mutex_lock(&worker_lock);
    available = result_available;
    if (available)
        *out = published_result;
    pthread_mutex_unlock(&worker_lock);
    return available != 0;
}

void thd_worker_stop(void)
{
    pthread_t thread;
    pthread_mutex_lock(&worker_lock);
    if (!worker_running) {
        pthread_mutex_unlock(&worker_lock);
        return;
    }
    worker_stop_requested = 1;
    thread = worker_thread;
    pthread_mutex_unlock(&worker_lock);
    pthread_join(thread, NULL);
    pthread_mutex_lock(&worker_lock);
    worker_running = 0;
    result_available = 0;
    pthread_mutex_unlock(&worker_lock);
}
