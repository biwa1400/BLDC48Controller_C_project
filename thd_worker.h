#ifndef THD_WORKER_H
#define THD_WORKER_H

#include "thd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One worker thread processes all 22 current channels.
 * Each channel independently estimates its fundamental frequency
 * and calculates THD and H1-H10 harmonic amplitudes.
 *
 * DMA must be initialized before starting the worker.
 * sample_rate_hz is the actual FIR output sample rate,
 * not the ADC or PWM frequency.
 *
 * Returns 0 on success, -1 on failure.
 */
int thd_worker_start(float sample_rate_hz);

/* Copy the latest complete THD result snapshot.
 *
 * Each channel contains:
 * - Independently estimated fundamental frequency
 * - THD percentage
 * - H1-H10 harmonic amplitudes
 * - THD validity bit
 * - Harmonic validity mask
 *
 * Channels without sufficient current or valid measurements
 * are marked invalid and should be displayed as N/A.
 *
 * Returns false until a measurement snapshot is available.
 * Thread-safe.
 */
bool thd_worker_get_result(ThdResult *out);

/* Stop and join the THD worker thread.
 * Safe to call if the worker has not been started.
 *
 * Does not release or unmap DMA resources.
 */
void thd_worker_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* THD_WORKER_H */
