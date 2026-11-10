# little_eyes —— 仿生眼睛渲染库（预编译发行版）

本目录是 **little_eyes（Uncanny Eyes）的发行版**，随本工程一起开源，**不含库源码**，
只包含别人编译本工程所需的最小集合：

```
little_eyes/
├── include/                          公共头文件（API / 移植接口 / 配置 / 素材声明）
│   ├── eyes_core.h                   主 API：初始化、表情、眨眼、瞳孔、渲染
│   ├── eyes_port.h                   跨平台移植接口（eyes_backend_t）
│   ├── eyes_config.h                 编译期配置（屏幕尺寸 / 圆屏裁剪 / 分块高度 …）
│   ├── eyes_fb.h                     通用帧缓冲后端
│   └── eye_source.h                  素材声明（bigBlue 眼球定义、虹膜纹理、眼睑表）
├── lib/esp32p4/liblittle_eyes.a      ESP32-P4 预编译静态库
└── lib/esp32p4/build_info.txt        该库的构建信息（目标 / 编译器 / 时间 / 参数来源）
```

别人克隆本工程后 **无需任何额外依赖**，`idf.py build` 即可编译。

## 我该怎么用它

工程根 `CMakeLists.txt` 已把它注册为组件：

```cmake
set(LITTLE_EYES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/little_eyes" CACHE PATH "...")
list(APPEND EXTRA_COMPONENT_DIRS "${LITTLE_EYES_DIR}")   # 必须在 project() 之前
```

业务代码直接 include 头文件即可，例如本工程的
`main/boards/dooirobot/app_ui/pages/uncanny_eyes/eyes_port_disp_hal.c`
就是"库 <-> 屏驱动"的桥接层。

## 换芯片 / 换 IDF 版本

预编译库与 **目标芯片 + IDF 版本 + 编译选项** 绑死。当前只提供：

| 目录             | 目标    | 编译器                 | IDF    |
| ---------------- | ------- | ---------------------- | ------ |
| `lib/esp32p4/` | esp32p4 | riscv32-esp-elf 14.2.0 | v5.5.2 |

其它芯片（如 esp32s3）需要自行补一个 `lib/<IDF_TARGET>/liblittle_eyes.a`，
方法见下一节。

## 重新生成预编译库（需要库源码）

1. 拿到库源码目录（含 `src/`、`tools/build_prebuilt.py`）。
2. 让目标工程先用**源码模式**构建一次，得到 `build/compile_commands.json`：
   ```bash
   idf.py build -DLITTLE_EYES_PREBUILT=OFF -DLITTLE_EYES_DIR=<库源码目录>
   ```
3. 用工程的真实编译命令产出 `.a`（保证 ABI 一致）：
   ```bash
   python <库源码目录>/tools/build_prebuilt.py ^
          --compile-commands <工程>/build/compile_commands.json ^
          --output <工程>/little_eyes/lib/<IDF_TARGET>/liblittle_eyes.a
   ```
4. 工程改回预编译模式：
   ```bash
   idf.py fullclean
   idf.py build
   ```

## 源码模式（调试库时用）

```bash
idf.py build -DLITTLE_EYES_PREBUILT=OFF -DLITTLE_EYES_DIR=<库源码目录>
```

该开关是 CMake cache 项，用完记得改回来（或直接 `idf.py fullclean`）。

## License

MIT。预编译库由同名源码编译而来，源码仓库见工程 README 的说明。
