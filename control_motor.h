// control_motor.h
#ifndef CONTROL_MOTOR_H
#define CONTROL_MOTOR_H

#include <stdint.h>

// ================= 公共 API =================

// 初始化电机控制寄存器映射
// 返回 0 表示成功，非 0 表示失败
int motor_ctrl_init(void);

// 启动运行
void run(void);

// 停止运行
void stop(void);

#endif // CONTROL_MOTOR_H
