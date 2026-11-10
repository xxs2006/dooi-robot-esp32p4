# Dooi ESP32-P4 桌面机器人

基于开源硬件方案复刻的桌面陪伴机器人，ESP32-P4 平台，带表情眼睛、数码时钟、摄像头视觉与语音交互。

本仓库在原开源工程基础上完成了**显示子系统重构**与**摄像头画面校正**，可直接编译烧录。硬件原理图、组装指南与物料清单见 [`hardware/`](hardware/)。

---

## 硬件平台

| 项 | 规格 |
|---|---|
| 主控 | ESP32-P4（RISC-V 双核，rev v1.3） |
| 屏幕 | 2.0 寸 IPS 裸屏，**ST7789** 控制器，240×320 物理分辨率 |
| 屏幕接口 | SPI，12P FPC 座（本方案使用右侧 FPC1：SCLK/MOSI/DC/RST） |
| 摄像头 | 板载摄像头模组，经 PPA 硬件做缩放与格式转换 |
| 存储 | 32MB Flash |
| 音频 | 板载麦克风 + 扬声器，支持语音唤醒与对话 |
| 外设 | 双舵机（耳部动作）、红外、触摸 |

> 原厂硬件配的是 1.28 寸圆形 LCD（GC9A01，240×240，双屏）。本仓库改用了 2.0 寸方屏，屏幕相关的移植说明见下文。

---

## 本仓库的改造内容

### 1. 单屏显示移植（ST7789）

原工程跑双 GC9A01 圆屏，改为单块 ST7789 方屏后一直「背光亮、全黑」。根因是两处配置与这块面板不匹配：

- **初始化序列**：IDF 内置驱动只发 4 条命令，缺少 VCOM、VRH、栅压、porch、帧率、gamma 等关键参数
- **SPI 模式**：面板实际工作在 **Mode 3**（时钟空闲高、上升沿采样），而工程默认是 Mode 0

现在改为**裸 panel IO + 逐条照抄厂家 ST7789v2 初始化序列**，不再经过 IDF 内置 `esp_lcd_panel_st7789`，避免它覆盖已写好的寄存器。

关键参数对照：

| 命令 | 本方案（ST7789v2） | 旧参数（T200H7） |
|---|---|---|
| `0xB2` porch | `0C 0C 00 33 33` | `05 05 00 33 33` |
| `0x20` 反显关闭 | 有 | 无 |
| `0xB7` 栅极电压 | `0x56` | `0x35` |
| `0xBB` VCOMS | `0x18` | `0x21` |
| `0xC3` VRH | `0x1F` | `0x0B` |
| `0xC6` 帧率 | `0x0F` | `0x01` |
| `0xE7` 双通道 SPI | `0x00` 关闭 | 无 |

### 2. 横屏显示

分辨率 **320×240**，MADCTL 取 `0x60`。横屏只有 `0x60` 与 `0xA0` 两个方向，二者互为 180°。

方向不对时只改一个宏：

```
left/right mirrored  -> 0x20 或 0x60 互换
upside down          -> 0xA0
```

配置入口在 `main/boards/dooirobot/app_ui/display_hal/display_hal.h` 顶部。

### 3. 单屏双眼动画 + 残影修复

`little_eyes` 是预编译库，画布固定 240×240，无法改尺寸。方案是把每只眼 2:1 降采样为 120×120，左右并排贴在 320 宽的屏上，居中显示。

原实现的脏区增量刷新每次只推 1~3 行，与缩放叠加后产生明显残影。现改为**影子帧缓冲**：

- `hal_flush()` 只写入内存影子缓冲，不直接碰屏幕
- 一帧渲染结束后，把所有变化的行合并成一整块连续区间，一次性原子推送

代价是一次传输的数据量变大，换来画面完全干净、无撕裂。

### 4. 数码时钟

原实现用点阵字体渲染，六位数字在 320 宽里放不下且没有冒号。改为**用 LVGL 矩形手绘七段数码管**，每位独立控制，冒号用两个方点。

- 单个数字 42×96，笔画 8px
- 六位总宽 304，水平居中
- 使用本地时区时间（`localtime`）

### 5. 摄像头画面旋转

摄像头模组在结构上装歪了，画面需要旋转。利用 **PPA（像素处理加速器）的硬件旋转**能力，在既有的缩放/格式转换流程里顺手完成，不占 CPU、不额外占内存、不掉帧。

- 仅**屏幕显示**这一路旋转，人脸识别与手势识别两路保持原方向（模型对方向不敏感，旋转反而会导致检测框坐标错位）
- 画面旋转后，人脸框的坐标映射同步旋转，否则框会固定偏在错误位置

### 6. 编译配置修复

ESP32-P4 rev v1.3 的内部 RAM 布局较紧，默认的小数据段优化会导致 `.sdata` 溢出、链接失败。已在工程里固化两处修改：

- `CMakeLists.txt`：追加 `-msmall-data-limit=0`，不把小变量塞进 gp 窗口
- `sdkconfig.defaults.esp32p4`：`CONFIG_COMPILER_OPTIMIZATION_SIZE=y`（`-Os`）

两者都已纳入版本控制，克隆后无需手动调整。

---

## 目录结构

```
.
├── CMakeLists.txt              # 含 -msmall-data-limit=0 修复
├── sdkconfig.defaults          # 通用默认配置
├── sdkconfig.defaults.esp32p4  # P4 专用配置（含 -Os）
├── hardware/                   # 硬件资料
│   ├── SCH_ESP32P4-Robot-Lite_v1.1.pdf
│   ├── SCH_ESP32P4-Robot-Lite_v1.2.pdf
│   ├── DooiRobot_Assembly_Guide_V1.0.pdf
│   ├── ESP32P4-Robot_lite_V1.1_Intro.pdf
│   └── DooiRobot_V2_BOM_V1.1.xlsx
├── main/
│   ├── boards/dooirobot/
│   │   ├── app_ui/
│   │   │   ├── display_hal/          # 屏幕驱动层（重点改造）
│   │   │   └── pages/
│   │   │       ├── uncanny_eyes/     # 眼睛动画 + 影子帧缓冲
│   │   │       ├── digital_clock/    # 七段数码时钟
│   │   │       └── camera/           # 摄像头页面与人脸框映射
│   │   ├── app_vision/               # 摄像头采集、PPA 缩放旋转
│   │   └── component/little_eyes/    # 眼睛渲染预编译库
│   ├── protocols/                    # 通信协议
│   └── assets/                       # 音频、表情等资源
├── docs/
└── scripts/
```

---

## 编译与烧录

**环境要求**：ESP-IDF **v5.5.2**（其他版本未经验证）

```bash
# 激活环境
. $IDF_PATH/export.sh            # Linux / macOS
# . \path\to\esp-idf\export.ps1  # Windows PowerShell

# 设置目标芯片
idf.py set-target esp32p4

# 编译
idf.py build

# 烧录并监视
idf.py -p <PORT> flash monitor
```

首次编译会自动拉取 `managed_components` 下的依赖（约 690MB），耗时较长。

**关于 `sdkconfig`**：该文件已在 `.gitignore` 中排除，避免带上个人 WiFi 配置。需要自定义时从 `sdkconfig.defaults.esp32p4` 生成即可。

---

## 显示调参入口

所有屏幕相关参数集中在 `main/boards/dooirobot/app_ui/display_hal/display_hal.h` 顶部：

| 宏 | 作用 |
|---|---|
| `LCD_H_RES` / `LCD_V_RES` | 分辨率（当前 320×240） |
| `DISP_ST_MADCTL` | 扫描方向，横屏 `0x60` / `0xA0` |
| `DISP_SPI_MODE` | SPI 模式（本面板必须为 3） |
| `DISP_SPI_CLK_HZ` | SPI 时钟（当前 20MHz） |
| `DISP_INVERT_COLOR` | 反显开关 |
| `DISP_MIRROR_X` / `DISP_MIRROR_Y` | 镜像 |

改完只需重新 `idf.py build flash`，无需动驱动代码。

---

## 硬件资料说明

`hardware/` 下的原理图、组装指南、板卡简介与物料清单**版权归原硬件厂商所有**，此处收录仅用于个人复刻与学习参考，不得用于商业用途。如需商用或二次分发，请联系原厂获取授权。

本仓库不含 PCB 工程源文件（`.kicad_pcb` / `.sch` / `.brd`），仅有 PDF 格式的原理图。

---

## 许可证

代码部分沿用原工程的 **MIT License**，版权声明见 [LICENSE](LICENSE)，保留原作者信息：

- Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd.
- Copyright (c) 2025 Project Contributors

本仓库对其所做的修改同样以 MIT 协议开放。

---

## 致谢

- 原始固件与硬件方案：深圳新智未来科技（Dooi Robot）
- 屏幕驱动参考：面板厂商提供的 ST7789v2 驱动包
- 眼睛动画渲染：`little_eyes` 预编译库
