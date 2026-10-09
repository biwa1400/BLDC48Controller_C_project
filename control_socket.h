
#ifndef CONTROL_SOCKET_H
#define CONTROL_SOCKET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t (*control_callback_t)(uint8_t cmd);
typedef uint8_t (*lut_load_callback_t)(const char *filename);

void start_control_server(int port, control_callback_t callback);
void stop_control_server(void);
void register_lut_load_callback(lut_load_callback_t callback);

#ifdef __cplusplus
}
#endif

#endif // CONTROL_SOCKET_H
