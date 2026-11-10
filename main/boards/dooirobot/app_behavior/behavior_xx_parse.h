#ifndef BEHAVIOR_XX_PARSE_H_
#define BEHAVIOR_XX_PARSE_H_

#include "behavior_channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 各硬件通道实现（在对应 .c 中定义） */
extern const behavior_channel_t behavior_channel_eye;
extern const behavior_channel_t behavior_channel_led;
extern const behavior_channel_t behavior_channel_servo;
extern const behavior_channel_t behavior_channel_motor;
extern const behavior_channel_t behavior_channel_audio;
extern const behavior_channel_t behavior_channel_iris;

#ifdef __cplusplus
}
#endif

#endif /* BEHAVIOR_XX_PARSE_H_ */