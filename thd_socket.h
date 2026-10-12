#ifndef THD_SOCKET_H
#define THD_SOCKET_H

/* Dedicated TCP service for completed 22-channel THD result snapshots.
 * This service does not access FPGA DMA memory or update the THD analyzer.
 * Start thd_worker before starting this service.
 *
 * Default TCP port; change if the port is already occupied.
 */
#define THD_SOCKET_PORT 10001

#ifdef __cplusplus
extern "C" {
#endif

/* Start the THD TCP service in one background thread.
 * Returns 0 on success, -1 on failure.
 */
int thd_socket_start(void);

/* Request shutdown and wait for the TCP service thread to finish.
 * Call before thd_worker_stop(). Safe if the service was not started.
 */
void thd_socket_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* THD_SOCKET_H */
