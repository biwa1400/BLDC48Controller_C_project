#ifndef CONTROL_SOCKET_H
#define CONTROL_SOCKET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 回调函数类型：输入命令字节，返回状态字节
typedef uint8_t (*control_callback_t)(uint8_t cmd);

// 启动服务器
void start_control_server(int port, control_callback_t callback);

// 停止服务器
void stop_control_server(void);

#ifdef __cplusplus
}
#endif

#endif // CONTROL_SOCKET_H
