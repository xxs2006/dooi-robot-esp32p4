#ifndef EYES_H
#define EYES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "eyes_config.h"   /* SCREEN_WIDTH / SCREEN_HEIGHT / EYES_* 配置 */

#ifdef __cplusplus
extern "C" {
#endif

#define FIXED_POINT_SHIFT 12
#define FIXED_POINT_ONE   (1 << FIXED_POINT_SHIFT)              // 使用 Q20.12 定点数格式，1.0f == (1 << 12)
#define FLOAT_TO_FIXED(f) ((int32_t)((f) * FIXED_POINT_ONE))    // 将浮点数转换为定点数
#define FIXED_MUL(a, b) ((int32_t)(((int64_t)(a) * (b)) >> FIXED_POINT_SHIFT))  // 定点数乘法

#define BORDER_COLOR  0x2147                                    // 眼睑描边
#define BORDER_WIDTH  10
#define F_EPS_FIXED   FLOAT_TO_FIXED(0.95f)

/* Screen dimensions: 见 eyes_config.h 中的 EYES_SCREEN_WIDTH / EYES_SCREEN_HEIGHT
 * （默认 240x240，可用编译选项 -DEYES_SCREEN_WIDTH=... 覆写） */

/* ======================= 圆屏列裁剪 =======================
 * 240x240 的 GC9A01 是圆屏，四角永远不可见。开启后每个分块只渲染/传输
 * 圆内的列区间，可省下约 15% 的像素计算和 SPI 数据量（块高 20 行时的并集
 * 窗口，理论上界 1-pi/4 = 21.5%）。
 *
 * 安全性：裁剪窗口取 [cx-hw, cx+hw)，关于屏幕中心对称。而面板的 MADCTL
 * 镜像对 240 列是 x -> 239-x，该区间恰好是自映射的，因此镜像模式下窗口与
 * 内容的相对关系完全不变，不会错位。
 *
 * 注意：如果你的屏是方屏，必须把 EYES_ROUND_CLIP 改为 0，否则四角会被裁掉。
 * ========================================================== */
#ifndef EYES_ROUND_CLIP
#define EYES_ROUND_CLIP    1
#endif

/* 可见圆域半径（像素）。240 圆屏为 120；若外壳有遮挡可略微调小（如 118）。 */
#ifndef EYES_CIRCLE_RADIUS
#define EYES_CIRCLE_RADIUS (SCREEN_WIDTH / 2)
#endif

typedef enum {
    EYE_RIGHT,
    EYE_LEFT,
    EYE_COUNT
} eyes_idx_t;

typedef enum{
    DIS_SAME = 0,    // 同线
    DIS_MIRROR ,     // 镜像
    DIS_SINGLE,
    DIS_COUNT,
}eyes_disp_mode_t;

typedef enum{
    EYES_LID_CLASSIC,
    EYES_LID_OPEN,
    EYES_LID_ANGRY,
    EYES_LID_ANGRY2,
    EYES_LID_SAD,
    EYES_LID_SUSPICIOUS,
    EYES_LID_ANGET_HAPPY,
    EYES_LID_CHEERFUL,
    EYES_LID_COUNT,
}EYES_LIDS_DEFINE;

typedef enum {
    MAT_BACKGROUND,
    MAT_SCLERA,
    MAT_IRIS,
    MAT_PUPIL
} MaterialType;

// 表情预设
typedef struct {
    EYES_LIDS_DEFINE left_lids;      // 使用的眼睑资源类型
    EYES_LIDS_DEFINE right_lids;
    float left_upper;                // 上眼睑目标开度 [0..1]
    float left_lower;                // 下眼睑目标开度 [0..1]
    float right_upper;
    float right_lower;
    float left_scale;                // 虹膜+瞳孔整体 scale [0.5..2.0]
    float right_scale;

    float pupil;                     // 瞳孔收缩量 [0..1]，小=大瞳孔

    float left_posX;                 // 目标位置 x（-1..1，左负右正）
    float left_posY;                 // 目标位置 y（-1..1，上负下正）
    float right_posX;
    float right_posY;

    uint16_t easePosMs;              // 位置平滑时长
    uint16_t easeScaleMs;            // scale 平滑时长
    uint16_t easePupilMs;            // pupilAmount 平滑时长

    uint8_t displayMode;       // 显示模式（如镜像）  
    bool autoMove;                   // 开关：自动扫视
    bool autoBlink;                  // 开关：自动眨眼
    bool autoPupils;                 // 开关：自动瞳孔
    bool autoAlive;                  // <--- 新增：是否开启仿生学眼睑引擎
    bool disableUpperJitter;         // 开关：禁用上眼睑微动联动
    bool disableLowerJitter;         // 开关：禁用下眼睑微动联动
} EyeExpressionPreset;

// 用于保存渲染时的关键状态
typedef struct {
    float x, y;
    float upperLidTarget, lowerLidTarget;
    float pupilAmount;
    float scale;
    float blinkFactor;
} EyeRenderState;

typedef struct {
    const uint16_t *data;
    uint16_t width;
    uint16_t height;
} Image;

// 每只眼的状态
typedef struct {
    bool inMotion;
    float eyeOldX;
    float eyeOldY;
    float eyeNewX;
    float eyeNewY;
    uint32_t moveStartTimeMs;
    uint32_t moveDurationMs;
    uint32_t lastSaccadeStopMs;
    uint32_t saccadeIntervalMs;

    // 眨眼随机定时
    uint32_t timeToNextBlinkMs;
    uint32_t timeOfLastBlinkMs;

    // 自动 pupilAmount 基础值（独立控制瞳孔大小）
    uint16_t irisFrame;
    float pupilAmount;

    // 瞳孔（pupilAmount）动画（独立通道）
    bool pupilResizing;
    float pupilStart;
    float pupilTarget;
    uint32_t pupilStartTimeMs;
    uint32_t pupilDurationMs;

    // 虹膜+瞳孔整体缩放 scale 动画（独立通道）
    bool scaleResizing;
    float scaleStart;
    float scaleTarget;
    uint32_t scaleStartTimeMs;
    uint32_t scaleDurationMs;

    int fixate;

    // --- 新增：眯眼平滑过渡 ---
    float currentSquint; 

    // --- 新增：眼睑基础开度动画 (独立通道) ---
    bool lidResizing;
    float upperLidStart;
    float lowerLidStart;
    float currentUpperBase;
    float currentLowerBase;
    uint32_t lidStartTimeMs;
    uint32_t lidDurationMs;

    // --- 追加：情绪应激微动 (Emotional Twitch) ---
    bool twitchActive;
    uint32_t twitchStartTimeMs;
    float twitchAmplitude; 

    // --- 新增：存放全局计算出的仿生眼睑偏移量 ---
    float bionicOffsetUpper;
    float bionicOffsetLower;
    float bionicOffsetPupil;  // <--- 新增：瞳孔特效偏移量
} OverallState;

typedef enum {
    NOT_BLINKING,
    BLINK_CLOSING,
    BLINK_OPENING
} BlinkState;

typedef struct {
    BlinkState state;
    uint32_t durationMs;
    uint32_t startTimeMs;
    float blinkFactor;
} EyeBlink;

typedef struct {
    uint16_t color;
    uint16_t slitRadius;
    float min;
    float max;
} PupilParams;

typedef struct {
    uint16_t radius;
    Image texture;
    uint16_t color;
    uint16_t startAngle;
    float spin;
    uint16_t iSpin;
    uint16_t mirror;
} IrisParams;

typedef struct {
    Image texture;
    uint16_t color;
    uint16_t startAngle;
    float spin;
    uint16_t iSpin;
    uint16_t mirror;
} ScleraParams;

typedef struct {
    const uint8_t *upper; // 每列两个字节：openY, closeY
    const uint8_t *lower;
    uint16_t color;
} EyelidParams;

typedef struct {
    uint16_t mapRadius;
    const uint8_t *angle;
    const uint8_t *distance;
} PolarParams;

// 用于存储预计算的极坐标（角度和原始距离）
typedef struct {
    uint16_t angle;       // 角度值 (0-1023)
    uint8_t  distanceRaw; // 原始距离值 (0-255)
} PolarCoord;

/*
 * 帧间脏检测 key。
 * 关键点：下列量都是"整数级"的，亚像素级的眼球位移 / 瞳孔抖动 / 眼睑呼吸
 * 并不会改变任何一个采样坐标，因此不会改变最终像素 —— 把这些量作为 key，
 * 就能安全地跳过大量"算完了但画面一模一样"的帧。
 */
typedef struct {
    int32_t  ix, iy;                   // 整数化眼球位置（决定极坐标采样起点）
    int32_t  s_fixed;                  // scale 定点值
    int32_t  irisValue_fixed;          // 决定虹膜尺寸与虹膜纹理采样
    uint16_t irisAngle, scleraAngle;   // 纹理旋转角
    const void *upperLid, *lowerLid;   // 当前眼睑资源
    const void *irisTex, *scleraTex;   // 当前纹理
    uint8_t  borderUpper, borderLower; // 眼睑描边开关
    uint8_t  valid;
    uint16_t lidY[SCREEN_WIDTH * 2];   // 上一帧上眼睑 Y[0..239] + 下眼睑 Y[0..239]
} EyeDirtyKey;

typedef struct {
    char name[16];
    uint16_t radius;
    uint16_t backColor;
    bool tracking;
    float squint;
    const uint8_t *displacement;
    PupilParams pupil;
    IrisParams iris;
    ScleraParams sclera;
    EyelidParams eyelids;
    PolarParams polar;
    PolarCoord *polar_lut; 

    /* 象限极坐标表：用 mapRadius x mapRadius 的象限表替代 2R x 2R 的全景表。
       mapRadius=240 时内存 900KB -> 173KB，cache 命中率大幅提升（原表必然
       分配在 PSRAM 上，逐像素随机访问会疯狂 miss）。

       进阶：polar_quad 再按 8x8 分块（swizzle）重排。原因是 Q1/Q3 沿屏幕行
       是行向访问、Q2/Q4 沿屏幕行是列向访问（moff = qx*R + qy，跨 480B），
       列向时每像素都要拉一条新的 64B cache line。分块后两种方向都落在同一个
       128B 的 tile 内，"每像素一条 line" 变成 "每 8 像素两条 line"。
       表大小和原来完全一样（R*R*2B），只是索引方式变了。 */
    uint16_t *polar_quad;    // [ceil(R/8)^2 * 64]，8x8 分块: (angle8 << 8) | dist
    uint8_t  *polar_dist_T;  // [R*R]: dist 的转置，仅第 2 象限使用（线性索引，勿分块）
    uint16_t  quad_tiles_per_row;   // ceil(mapRadius / 8)，分块索引用

    uint8_t *displacement_transposed; // 转置后的位移图
} EyeDefinition;

/* =========================================================================
 * 脏区 run：一段连续的需要重绘的行区间，带自己的列窗口。
 * 一个 run 通常对应 1~3 行，一次 SPI 传输。
 * ========================================================================= */
#ifndef EYES_DIRTY_MAX_RUNS
/* 上限 = ceil(SCREEN_HEIGHT / 2)：最坏情况是脏行与净行严格交替。
 * 放在 Eye 结构里（静态 BSS）而不是栈上 —— 眼睛任务只有 4KB 栈，
 * 120 项 × 8B = 960B 太占地方。 */
#define EYES_DIRTY_MAX_RUNS  120
#endif

typedef struct {
    int16_t y0, y1;     /* 行区间 [y0, y1) */
    int16_t x0, x1;     /* 该 run 的列区间 [x0, x1) */
} DirtyRun;

typedef struct {
    EyeDefinition *definition;
    float x;
    float y;
    uint16_t currentIrisAngle;
    uint16_t currentScleraAngle;
    EyeBlink blink;

    // 渲染时的平滑开度
    float upperLidFactor;
    float lowerLidFactor;

    // 用户设置的目标开度（每只眼独立）
    float upperLidTarget;
    float lowerLidTarget;

    // 为每只眼睛独立存储当前使用的眼睑资源指针
    const uint8_t *currentUpperLid;
    const uint8_t *currentLowerLid;

    bool drawAll; // 下帧是否全覆盖刷新

    float scale;  // 虹膜+瞳孔整体缩放

    OverallState state;

    EyeRenderState lastRenderState; // 保存上一帧的渲染状态
    EyeDirtyKey    dirtyKey;        // 帧间脏检测（精确跳过 + 脏行范围）

    /* 逐行脏检测的暂存区。每只眼一份，两个渲染任务各用各的，不能做成文件级
     * static（两只眼的任务同核同优先级，会互相踩）。不放栈上是因为眼睛任务
     * 只有 4KB 栈，而这里要 ~1KB。 */
    int16_t rowXMin[SCREEN_HEIGHT];  // 每行脏列的左闭界
    int16_t rowXMax[SCREEN_HEIGHT];  // 每行脏列的右闭界（-1 = 该行无变化）

    /* run 输出缓冲，同上理由放静态区 */
    DirtyRun dirtyRuns[EYES_DIRTY_MAX_RUNS];
} Eye;

typedef struct {
    Eye eyes[2];
    uint8_t eyeIndex;
    uint8_t eye_disp_mode;
    uint8_t eye_disp_next_mode;
    uint8_t eye_emotion_type;
    bool autoMove;
    bool autoBlink;
    bool autoPupils;  
    bool autoAlive;                  // <--- 新增：是否开启仿生学眼睑引擎
    bool disableUpperJitter;         // <--- 新增
    bool disableLowerJitter;         // <--- 新增
    uint16_t maxGazeMs;
    int16_t nufix;
} EyeController;

typedef enum{
    EYES_IRIS_DEFAULT,
    EYES_IRIS_EMERALD, // 翡翠绿
    EYES_IRIS_VIOLET,  // 紫色
    EYES_IRIS_BLUE,    // 蓝色
    EYES_IRIS_AMBER,   // 黄色
    EYES_IRIS_GRAY,    // 灰色
    EYES_IRIS_RUBY,    // 橙色
    EYES_IRIS_WINERED, // 酒红色
    EYES_IRIS_COUNT,
}EYES_IRIS_DEFINE;

typedef enum {
    EYES_EMOTION_DEFAULT,
    EYES_EMOTION_FOCUS_BELOW_CENTER,
    EYES_EMOTION_FOCUS_TOP_CENTER,
    EYES_EMOTION_FOCUS_TOP_LEFT,
    EYES_EMOTION_FOCUS_TOP_RIGHT,
    EYES_EMOTION_FOCUS_LEFT,
    EYES_EMOTION_FOCUS_RIGHT,
    EYES_EMOTION_HAPPY,
    EYES_EMOTION_HAPPY1,
    EYES_EMOTION_HAPPY2,
    EYES_EMOTION_ANGRY,
    EYES_EMOTION_ANGRY1,
    EYES_EMOTION_ANGRY_RIGHT,
    EYES_EMOTION_ANGRY_LEFT,
    EYES_EMOTION_SAD, 
    EYES_EMOTION_SUSPICIOUS,
    EYES_EMOTION_AMAZED,
    EYES_EMOTION_COUNT,
    EYES_EMOTION_CUSTOM,   /* 哨兵：非预设情绪，编辑器/真机桥经 eye_controller_apply_preset() 使用 */
}EYES_EMOTION;
extern const EyeExpressionPreset g_emotion_presets[EYES_EMOTION_COUNT];

/* 析构：释放极坐标表 / 位移图等预分配内存，并把控制器状态清零。
 * 重新 eye_controller_init() 前不需要手动调用（init 内部会先 deinit）。 */
void eye_controller_deinit(void);

/* 让两只眼在下一帧整屏重绘（例如切页面、屏幕上电后） */
void eye_controller_force_redraw(void);

/* 显示模式切换：转发给后端的 set_mirror（DIS_MIRROR 不镜像，其余镜像）。
 * 返回 0。后端未实现时为 no-op。 */
int eyes_disp_mode_set(int idx, int mode);

/* 直接把一份表情预设应用到控制器（编辑器 / 真机桥 / 自定义表情入口）。
 * 会覆盖当前"情绪类型"，之后 eye_emotion_type 置为 EYES_EMOTION_CUSTOM。
 * 语义与 eye_controller_set_emotion() 内部完全一致，只是不查内置表情表。
 * 注意：与渲染线程并发修改状态时需要自行加锁（eyes_enter_critical/exit）。 */
void eye_controller_apply_preset(const EyeExpressionPreset *preset);

/* 独立调整单眼眼睑：切换眼睑形状 + 设定上下开度目标（各自 [0,1]）。
 * - durationMs > 0：沿用库内的平滑缓动（无情绪微颤，适合参数拖拽）；
 * - durationMs == 0：立即改当前开度（跳过缓动，也跳过平滑低通起点）。 */
void eye_controller_set_lid(uint32_t eye_index, EYES_LIDS_DEFINE lid_type,
                            float upperFactor, float lowerFactor, uint32_t durationMs);

float update_blink_state(Eye *eye);

void apply_auto_blink(void);
void apply_auto_pupilAmount(Eye *eye, uint32_t eyeIndex);
void apply_auto_move(void);
void apply_scale_animation(Eye *eye);
void apply_pupil_animation(Eye *eye);
void apply_auto_compute_eyelids(const Eye *eye, uint32_t eyeIndex, float *upperQ, float *lowerQ);

// 眨眼
void eye_controller_blink(uint32_t duration);      // 双眼
void eye_controller_wink(uint32_t index);          // 单眼

// 位置（双眼)
void eye_controller_set_target_position(float xTarget, float yTarget, int32_t durationMs);
void eye_controller_set_position(float x, float y);

// 位置（单眼)
void eye_controller_set_target_position_eye(uint32_t eyeIndex, float xTarget, float yTarget, int32_t durationMs);
void eye_controller_set_position_eye(uint32_t eyeIndex, float x, float y);

// pupil（瞳孔独立控制）
void eye_controller_set_target_pupil(float ratio, int32_t durationMs);
void eye_controller_set_pupil(float ratio);

void eye_controller_set_target_pupil_eye(uint32_t eyeIndex, float ratio, int32_t durationMs);
void eye_controller_set_pupil_eye(uint32_t eyeIndex, float ratio);

// 虹膜+瞳孔整体缩放（scale）
void eye_controller_set_eye_scale(float scale_left, float scale_right); // 立即设置双眼缩放

// 虹膜+瞳孔整体 scale 动画接口
void eye_controller_set_target_scale_eye(uint32_t eyeIndex, float ratio, int32_t durationMs);
void eye_controller_set_target_eye_scale(float scale_left, float scale_right, int32_t durationMs);

void eye_controller_init(EyeDefinition *def_left, EyeDefinition *def_right);
void eye_controller_set_emotion(EYES_EMOTION emotion);
void eye_controller_apply_iris_texture(EYES_IRIS_DEFINE iris_type);
bool eye_controller_render_frame(int idx);
EyeController *eye_controller_get_instance(void);

/* 性能统计：rendered / skipped 帧数，rows = 累计重绘行数，pixels = 累计传输像素 */
void eye_controller_get_perf(int idx, uint32_t *rendered, uint32_t *skipped,
                             uint32_t *rows, uint32_t *pixels);

void render_eye(Eye *eye, uint32_t eyeIndex, float upperFactor, float lowerFactor, float blinkFactor);

#ifdef __cplusplus
}
#endif

#endif