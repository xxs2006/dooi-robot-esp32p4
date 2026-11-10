# app_ui/ui_logic — 业务逻辑层说明

本目录是 dooirobot ESP32-P4 项目的 **UI 业务逻辑层**，位于渲染层（`app_ui/pages` / `display_hal`）与底层硬件/服务（`app_vision`、`app_behavior`、`app_motor`、`app_peripheral`）之间。

它的职责是：**把"底层事件"翻译成"机器人应该做什么"，并集中管理状态机、屏幕路由、行为分发与各类交互规则**，使渲染层只负责画、底层只负责执行。  

---

## 1. 总体架构

```
            ┌──────────── 外部事件源 ────────────┐
            │  app_vision(摄像头) / 触摸按键 /    │
            │  IMU / 电源 / 蓝牙 / Web / MCP      │
            └────────────────┬───────────────────┘
                             │  app_ui_logic_post(evt)
                             ▼
                    ┌─────────────────────┐
                    │    事件队列 +        │
                    │    logic_task(核0)  │   ← 单线程顺序消费，天然串行化
                    └─────────┬───────────┘
                             │  s_handler_table[evt.type]
                             ▼
        ┌──────────┬─────────┼──────────┬────────────┬───────────┐
        ▼          ▼         ▼          ▼            ▼           ▼
  logic_sys_status logic_peripheral logic_vision dooi_behavior face_tracker logic_timeout
   (系统/对话状态)  (按键/IMU/电源)   (视觉→行为)  (情绪状态机)  (人脸追踪)  (待机超时)
        │          │         │          │            │           │
        └──────────┴────┬────┴──────────┴────────────┴───────────┘
                        │  app_ui_logic_request_behavior(...)  /  screen_set_current(...)
                        ▼
              ┌──────────────────────┐
              │  app_behavior(场景)   │  ──► 眼睛/舵机/LED/音频/屏幕
              │  screen_manager(切屏)│
              └──────────────────────┘
```

### 核心设计原则

1. **事件驱动 + 单消费者**：所有输入统一封装成 `app_event_t` 进入 `s_evt_queue`，由 `logic_task`（绑定核 0，优先级 10）顺序处理，避免并发竞态。
2. **逻辑与渲染解耦**：逻辑层通过 `logic_timeout` 镜像当前屏幕，不直接访问 `screen_manager`；切屏用 `app_ui_logic_request_screen()` → `screen_set_current()`，由渲染线程消费 `next_screen` 标志。
3. **行为统一网关**：任何"让机器人动起来"的请求都经 `app_ui_logic_request_behavior()`（或等价 `dooi_behavior`），而非各模块直接调 `app_behavior_play_scene`，便于统一记录与未来做优先级裁决。
4. **心跳主循环旁路**：高频率的超时判定（待机、游戏回合、情绪切换）不走事件队列，由 UI 线程 25ms 主循环直接调用 `app_ui_logic_tick()` / `logic_page_eyes_game_timeout_check()` / `logic_emotion_auto_update()`，避免每 tick 一次跨核 round-trip。

---

## 2. 事件模型（`app_ui_logic.h/.c`）

### 事件类型 `app_event_type_t`

- **对话 / 系统状态**：`LOADING_PROTOCOL`、`CONNECT_WIFI`、`STANDBY`、`LISTENING`、`SPEAKING`、`CONNECTING`、`ACTIVATION`、`CHECKING_NEW_VERSION`、`ERROR`、`IDLE`
- **系统 / 连接**：`BLUETOOTH_OPEN/CLOSE`、`WEB_SERVER_CONNECT/DISCONNECT`、`CAMERA_TIGGRER_PREVIEW`、`IR_ENABLE/DISABLE/TRIGGER`、`BUTTON`、`POWER_BTN_PRESS`、`POWER_SHUTDOWN`、`IMU`、`POWER_STATUS`
- **视觉**：`VISION_TASK_EXITED/STARTED`、`FACE_DETECTED`、`FACE_ALIGNED`、`GESTURE`

### 事件分发表

`app_ui_logic.c` 中 `s_handler_table[APP_EVT_COUNT]` 把每个事件类型映射到对应处理函数（命名约定 `on_xxx`）。未注册/未知类型打印 `Unhandled event type` 警告。

### 关键公共接口

| 函数                                               | 作用                                                                                   |
| -------------------------------------------------- | -------------------------------------------------------------------------------------- |
| `app_ui_logic_init()`                            | 创建事件队列、注册视觉回调、初始化 face_tracker 与 dooi_behavior，并启动`logic_task` |
| `app_ui_logic_post(evt, from_isr)`               | 线程安全投递事件（支持 ISR 上下文）                                                    |
| `app_ui_logic_request_screen(screen)`            | 请求切屏（驱动行为状态：进入音乐页→`PAGE_MUSIC`，离开→`STANDBY`）                |
| `app_ui_logic_request_idle(trigger)`             | 控制渲染层待机计时器（trigger=1 触发待机，0 仅重置）                                   |
| `app_ui_logic_request_shutdown()`                | 异步请求整机关机（投递事件后立即返回，不阻塞调用方）                                   |
| `app_ui_logic_report_screen(cur, old)`           | UI 线程回报屏幕状态给逻辑层                                                            |
| `app_ui_logic_get_current_screen()`              | 读取逻辑层镜像的当前屏幕                                                               |
| `app_ui_logic_get_state()` / `is_conv_state()` | 查询当前对话状态 / 是否对话中                                                          |
| `app_ui_logic_tick()`                            | UI 心跳：推进超时状态机，必要时触发待机→时钟页                                        |

### 全局状态 `g_app_state`

- `current_state` / `prev_state`：对话/系统状态机
- `sys_is_init_done`：系统初始化完成标志
- `is_conv_state`：是否处于 `LISTENING` 或 `SPEAKING`（多模块据此抑制自动行为）

---

## 3. 各子模块业务逻辑

### 3.1 `logic_sys_status` — 系统/对话状态机

负责所有 `APP_EVT_STATE_*` 与连接类事件的响应，是机器"身份状态"的大脑。

- 维护系统初始化完成标志，打上对应系统/指示灯状态。
- **激活（ACTIVATION）**：启动视觉任务、进入交互就绪。
- **待机（IDLE）**：切换时钟屏幕、启动人脸检测。
- **监听/说话（LISTENING/SPEAKING）**：设置对话态标志 `is_conv_state`，驱动眼睛进入交互情绪（见 `logic_emotion`）。
- **加载协议 / 连 WiFi / 检查版本 / 错误**：对应 LED/屏幕提示与日志。
- **蓝牙 / Web 连接事件**：更新连接指示。
- 初始化阶段通过 `dooi_behavior_enable(true)` 打开自动情绪状态机。

### 3.2 `logic_peripheral` — 外设输入（按键/IMU/电源/IR）

处理 `BUTTON`、`IMU`、`POWER_BTN_PRESS`、`POWER_SHUTDOWN`、`POWER_STATUS`、`IR_*` 等：

- **按键 `on_button`**：根据当前屏幕把按键事件分发给对应页面逻辑（`logic_page_eyes/camera/music/digital_clock` 的 `xxx_on_key`）。
- **IMU `on_imu`**：倾斜方向驱动头部/身体姿态（如 `app_motor` 或行为场景）。
- **电源按键 `on_power_btn_press`**：进入关机确认流程。
- **关机 `on_power_shutdown`**：优雅关机（停对话 → 停行为 → 屏幕提示 → 切电源）。
- **电量 `on_power_status`**：充电/低电标志更新，低电时触发提示行为。
- **IR `on_ir_*`**：红外使能/触发，用于外接遥控或避障联动。

### 3.3 `logic_vision` — 视觉成果转行为

把视觉任务的结果翻译成机器人行为：

- **`on_vision_task_started/exited`**：启停 `app_vision`；退出时 `priority_boost(10)` 保证视觉任务先执行完。

  - 首次启动延迟 2s，再延迟 100ms，避免相机初始化未完成。
- **`on_face_detected`**：

  - `dooi_behavior_set_face_detected(true)` 刷新"有人脸"时间戳；
  - `face_tracker_push_result()` 把结果喂给人脸追踪器；
  - 设置行为状态为 `STANDBY`（进入待命表情）。
- **`on_face_aligned`**：打印对齐完成日志（单次追踪成功）。
- **`on_gesture`**：将视觉手势映射为行为场景：

  | 手势    | 触发场景                      |
  | ------- | ----------------------------- |
  | ONE     | `action_greeting`（打招呼） |
  | TWO     | `action_nod_agree`（点头）  |
  | THREE   | `action_dance`（跳舞）      |
  | FOUR    | `action_hug`（拥抱）        |
  | FIVE    | `action_welcome`（欢迎）    |
  | OK      | `happy` + `bounce_jump`   |
  | LIKE    | `happy`                     |
  | DISLIKE | `angry`                     |

  去抖：手势与上一帧相同、或 `score < 0.95` 时忽略。

### 3.4 `dooi_behavior` — 情绪自动切换状态机

机器人"无交互时的自发情绪"核心，周期驱动眼睛表情在 DEFAULT / FOCUS / HAPPY 之间流转。

- **状态转移**（均带随机切换间隔，避免机械感）：
  - `DEFAULT → FOCUS`：无脸超时（默认 20s）后进入"四处张望"焦点态。
  - `FOCUS → DEFAULT`：焦点态超过 20s 且无脸，回到默认。
  - `FOCUS/DEFAULT → HAPPY`：检测到人脸立即进入开心态（随机开心表情）。
  - `HAPPY → DEFAULT`：丢脸超过 `BEHAVIOR_HAPPY_NO_FACE_TIMEOUT_MS`(30s)。
- **抑制条件**：对话中（`is_conv_state`）或处于手动情绪时，`dooi_behavior_update()` 直接返回，让位给对话/交互情绪（`logic_emotion`）。
- `FACE_DETECTION_VALID_MS`(2s) 作为"是否还有人脸"的有效窗口。
- 提供统一行为网关 `app_ui_logic_request_behavior(scene, variant, prio)`：以 `status_` 前缀的场景走 `app_behavior_play_status`，其余走 `app_behavior_play_scene`，并记录最近场景名供调试。

### 3.5 `logic_emotion` — 眼睛情绪自动切换（交互态）

从 `eyes_control.c` 迁入的"情绪自动切换"决策，专门服务于**对话 / 手动情绪**场景：

- `logic_emotion_set_manual()`：设置一次手动情绪（手动标记置位，自动切换临时让位）。
- `logic_emotion_set_auto()`：设置自动产生情绪（手动标记清除）。
- `logic_emotion_auto_update()`（每节拍调用）：
  - **对话中**：从 `FOCUS_BELOW_CENTER / HAPPY1 / SAD / SUSPICIOUS` 随机切换；
  - **手动态**：从 `RANDOM_EMOTION_START~END` 范围内随机切换一次后清除手动标记；
  - **其他**：保持。
- 切换间隔在 `[MIN_SWITCH_INTERVAL_MS, MAX_SWITCH_INTERVAL_MS]` 间随机。

### 3.6 `face_tracker` — 人脸追踪（跨页面复用）

单例状态机，把"人脸在画面中的位置"转换为舵机转向指令，让人脸始终居中。

- **开机即启用**：`app_ui_logic_init()` 中 `face_tracker_init(NULL)` 之后必须显式 `face_tracker_enable(true, true)`。此时 cfg 为空，数据源走 `face_tracker_push_result()`，实现任意页面的自动人脸追踪。**`init(NULL)` 不会启用追踪**，漏掉这行会导致追踪完全不工作。
- **配置注入（依赖倒置）**：`face_tracker_init(cfg)` 注入 `get_result / map_display / on_status / on_aligned` 回调，使追踪器与具体页面解耦；**注入非空 cfg 时会重新武装状态机并启用**，因此进入相机页面即切换到页面内数据源。
- **页面卸载必做**：卸载时必须 `face_tracker_unbind()` 解绑回调，否则 `on_status` 持有悬空 LVGL 指针会崩溃。`unbind()` 只清回调、不停用追踪，跨页面追踪仍可通过 `face_tracker_push_result()` 继续；要完全停止（含停电机）用 `face_tracker_deinit()`。
- **双数据源**：页面内 `get_result` 回调，或跨页面 `face_tracker_push_result()`（视觉层检测到人脸即推送，缓存 500ms）。
- **状态机**：
  - `CORRECTING`：首次/重捕后先把脸转到死区内；超过 `CORRECT_TIMEOUT_MS`(4s) 未找到脸 → `LOST`。
  - `TRACKING`：持续微调；人脸丢失超过 `LOST_TIMEOUT_MS`(2s) → `LOST`。
  - `LOST`：重新检测到脸 → 回到 `CORRECTING`。
  - 单次模式（`continuous=false`）对齐后回调 `on_aligned` 并投递 `APP_EVT_FACE_ALIGNED`，随后自动停用。
- **转向算法**：`priv_send_turn` 计算偏移 → 死区 `DEAD_ZONE`(25px) 内不转 → 像素转角度 `0.25°/px`，限幅 `[MIN_ANGLE, MAX_ANGLE]`(3°~30°)，限速 `TURN_SPEED`，追踪间隔 `TRACK_INTERVAL_MS`(900ms)。

### 3.7 `logic_timeout` — 待机超时状态机（纯状态，无 OS 依赖）

- 持有 `current_screen / old_screen / idle_timeout_ticks`，是超时逻辑唯一状态持有方，便于 PC 单测。
- `logic_timeout_tick_advance()`：仅当处于 `SCREEN_EYES` 时累计 idle tick。
- `eyes_should_idle()`（纯判定）：在 `SCREEN_EYES` 且 idle 超过 `IDLE_TIMEOUT_MS` 后返回 true 并清零计数（先推进再判定语义）。

---

## 4. 页面级交互逻辑（`logic_page_*`）

每个页面一个 `*_on_key(key_id, event)` 入口，由 `logic_peripheral.on_button` 按当前屏幕分发。键位为左右触摸键（`TOUCH_BTN_KEY_LEFT/RIGHT`），事件含 `CLICK / DOUBLE_CLICK / LONG_PRESS`。

### 4.1 `logic_page_eyes` — 眼睛页（手势组合 + 内置小游戏）🌟 最复杂

除基础即时反馈外，维护一个**按键手势历史缓冲**（最多 8 条，20s 无操作自动 reset），按规则依次匹配：

**基础反馈**：单击左右键 → `action_focus_left/right` 或 `action_focus_top_left/top_right`（用 `PRIO_HIGH` 交互高优先级，确保打断普通/情绪场景）；`focus_type` 每 20s 空闲翻转一次，切换"平视/俯视"两套反馈。

**组合手势规则**（按匹配顺序）：

| 规则       | 触发条件                  | 行为                                                                       |
| ---------- | ------------------------- | -------------------------------------------------------------------------- |
| 单击+双击  | 3s 内 单击→双击（异键）  | `action_angry_focus_left/right`                                          |
| 单击+长按  | 5s 内 单击→长按          | 随机`angry` / `emotion_angry(flash_rage)` / `action_angry_focus_*`   |
| 长按       | 任意键长按                | 非对话态→`BEHAVIOR_STATUS_IDLE` 复位；对话态→`ApplicationStopChat()` |
| 双击       | 任意键双击                | `game_win`（笑声+惊喜音）                                                |
| 交替点击   | ≥3 次左右交替单击，3s 内 | 触发**交替游戏** `game_start`                                      |
| 三连击同键 | 3s 内同一键三击           | 左键→`curious`，右键→`thinking`                                      |
| 快速双单击 | 3s 内 两次单击            | `surprised`                                                              |

**内置交替游戏**（规则7 触发）：

- 3 个回合，每回合随机正确侧（左右轮流），由 `scenes/sys/game_round_{round}_{side}.json` 驱动虹膜/LED/舵机/表情。
- 答对进下一回合；3 回合全对 → `game_win`（绿光+庆祝）；答错 → `game_mismatch`（红虹膜/红LED/怒）；单回合超 `GAME_TIMEOUT_MS`(20s) → `game_timeout`（红LED/悲伤）后 `game_exit` 复位。
- 游戏进行中只响应 `CLICK`（交给 `game_handle_touch`），其余手势被吞掉。
- 防竞态：进入 `PLAYING` 前先写 `s_game_round_start_time`，再放开状态，避免渲染核轮询误判立即超时。

**公共接口**：`logic_page_eyes_reset_state()`（清手势缓冲/退出游戏）、`logic_page_eyes_is_game_playing()`、`logic_page_eyes_game_round_start_time()`、`logic_page_eyes_game_timeout_check()`（UI 心跳调用，超时→`game_timeout`）。

### 4.2 `logic_page_camera` — 相机页

- 左键单击：相机模式 −1（跳过 `PHOTO`）；双击：回页面选择；长按：切眼睛页。
- 右键单击：相机模式 +1（跳过 `PHOTO`）；双击：回页面选择；长按：切眼睛页。
- 相机页内的人脸追踪由 `face_tracker` 配合页面注入的回调驱动（`camera_on_vision_result_to_screen` 在 `on_vision_result` 中显示识别框）。

### 4.3 `logic_page_music` — 音乐频谱页

- 左右键双击：回页面选择；长按：切眼睛页。
- 进入/离开此页时由 `app_ui_logic_request_screen` 切换行为状态 `PAGE_MUSIC` / `STANDBY`。

### 4.4 `logic_page_digital_clock` — 数字时钟页

- 左键单击：切显示模式（`digital_clock_mode_change`）；双击：计时模式 −5 分钟，否则回眼睛页；长按：回眼睛页。
- 右键单击：切换走时（`digital_clock_ticking_toggle`）；双击：计时模式 +5 分钟，否则回眼睛页；长按：回眼睛页。

---

## 5. 关键数据流举例

### 5.1 待机 → 用户出现 → 开心

1. 眼睛页空闲超过 `IDLE_TIMEOUT_MS` → `app_ui_logic_tick()` 判定 `eyes_should_idle()` → 投递 `APP_EVT_STATE_IDLE` → 切时钟页 + 启动人脸检测。
2. 视觉层检测到人脸 → `on_vision_result` 投递 `APP_EVT_FACE_DETECTED`。
3. `on_face_detected`：刷新人脸时间戳 + 喂追踪器 + 置 `STANDBY`。
4. `dooi_behavior_update()` 检测到有人脸 → `DEFAULT/FOCUS → HAPPY`，眼睛变开心。

### 5.2 手势交互

1. 相机拍到手势 → `on_vision_result` 转 `APP_EVT_GESTURE`（去抖后）。
2. `on_gesture` 按表播放对应行为场景（经 `app_behavior_play_scene`）。

### 5.3 眼睛页手势小游戏

1. 左右交替单击 3 次 → `match_alternating_dance` → `game_start`。
2. UI 心跳每帧 `logic_page_eyes_game_timeout_check()` 检测回合超时。
3. 玩家按正确侧 → `game_handle_touch` 推进回合；3 回合 → `game_win` → `game_exit` 复位硬件。

---

## 6. 目录文件清单

| 文件                           | 职责                                 |
| ------------------------------ | ------------------------------------ |
| `app_ui_logic.h/.c`          | 事件队列、分发表、公共接口、全局状态 |
| `dooi_behavior.h/.c`         | 情绪自动切换状态机 + 行为统一网关    |
| `logic_sys_status.h/.c`      | 系统/对话状态机                      |
| `logic_peripheral.h/.c`      | 按键/IMU/电源/IR 等外设输入          |
| `logic_vision.h/.c`          | 视觉成果（人脸/手势）→ 行为         |
| `logic_emotion.h/.c`         | 交互态眼睛情绪自动切换               |
| `face_tracker.h/.c`          | 人脸追踪状态机（舵机转向）           |
| `logic_timeout.h/.c`         | 待机超时纯状态机（可单测）           |
| `logic_page_eyes.c`          | 眼睛页手势组合 + 内置小游戏          |
| `logic_page_camera.c`        | 相机页按键逻辑                       |
| `logic_page_music.c`         | 音乐页按键逻辑                       |
| `logic_page_digital_clock.c` | 数字时钟页按键逻辑                   |

---

## 7. 设计要点与注意事项

- **跨核竞态**：逻辑任务在核 0，渲染/行为在核 1；切屏、游戏启动等都通过"先写时间戳/状态再放开标志"消除竞态窗口。
- **页面卸载必做**：使用 `face_tracker` 的页面卸载时务必 `face_tracker_unbind()`，否则回调持有悬空 LVGL 指针会崩溃（典型崩溃点：`lv_label_set_text` → `lv_label_revert_dots` Store access fault）。
- **行为声音去重**：`logic_page_eyes` 中生气/胜利等场景的音效由对应 JSON 的 audio 通道统一播放，逻辑层不再直接调 `ApplicationPlaySound`，避免重复。
- **高优先级交互**：用户主动按键反馈用 `PRIO_HIGH`，确保能打断正在排队的普通/情绪场景，避免约 2s 延迟。
- **可测试性**：`logic_timeout` 与手势匹配函数尽量做成纯逻辑/纯判定，便于 PC 单测。
