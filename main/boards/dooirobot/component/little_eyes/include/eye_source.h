#ifndef EYE_SOURCE_H
#define EYE_SOURCE_H

#include "eyes_core.h"

extern EyeDefinition bigBlue;

extern const uint8_t disp_240_125[120 * 120];
extern const uint8_t polarDist_240_130_95_0[240 * 240];
extern const uint8_t polarAngle_240[240 * 240];

extern const uint8_t eye_open_uppper[480];
extern const uint8_t eye_open_lower[480];
extern const uint8_t eye_angry_left_upper[480];
extern const uint8_t eye_angry_left_lower[480];
extern const uint8_t eye_angry_right_upper[480];
extern const uint8_t eye_angry_right_lower[480];
extern const uint8_t eye_sad_upper[480];
extern const uint8_t eye_sad_lower[480];
extern const uint8_t eye_suspicious_upper[480];
extern const uint8_t eye_suspicious_lower[480];
extern const uint8_t eye_angry_happy_upper[480];
extern const uint8_t eye_angry_happy_lower[480];
extern const uint8_t eye_cheerful_upper[480];
extern const uint8_t eye_cheerful_lower[480];
extern const uint8_t eye_classic_upper[480];
extern const uint8_t eye_classic_lower[480];

extern const uint16_t eyeIris_default[7680];  // 256x30, 16 bit 565 RGB 蓝
extern const uint16_t eyeIris_emerald[7680];  // 绿
extern const uint16_t eyeIris_violet[7680] ;  // 紫
extern const uint16_t eyeIris_blue[7680]; // 蓝
extern const uint16_t eyeIris_amber[7680]; // 黄
extern const uint16_t eyeIris_gray[7680]; // 灰1
extern const uint16_t eyeIris_ruby[7680]; // 橙
extern const uint16_t eyeIris_winered[7680]; // 红

#endif