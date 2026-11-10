# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# 强制 Ninja 使用响应文件，解决 Windows 命令行过长问题
set(CMAKE_C_USE_RESPONSE_FILE_FOR_OBJECTS 1)
set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_OBJECTS 1)
set(CMAKE_NINJA_FORCE_RESPONSE_FILE 1, FORCE)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# little_eyes（Uncanny Eyes）：跨平台眼睛渲染库，随工程开源（发行版：头文件 + 预编译静态库）。
# 默认使用工程内的 little_eyes 组件（相对路径，克隆即可用，无需额外依赖）：
#   main/boards/dooirobot/component/little_eyes/（头文件 + 预编译静态库）。
# 想调试库源码时，把 LITTLE_EYES_DIR 指向库源码目录并加 -DLITTLE_EYES_PREBUILT=OFF：
#   idf.py build -DLITTLE_EYES_DIR=<库源码目录> -DLITTLE_EYES_PREBUILT=OFF
# 必须放在 project() 之前，构建系统在此处收集组件目录。
set(LITTLE_EYES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/main/boards/dooirobot/component/little_eyes" CACHE PATH "little_eyes 组件目录（内置预编译库；也可指向外部源码目录）")
list(APPEND EXTRA_COMPONENT_DIRS "${LITTLE_EYES_DIR}")

# ---- 规避 ESP32-P4 rev<3 的 .sdata 链接溢出 ----
# P4 rev<3 的内部 DRAM 被切成 sram_low(约179KB) + sram_high(384KB) 两块不连续区域；
# 而 RISC-V 的 gp 相对寻址窗口只有 ±2KB（合计 4KB）。本工程 .sdata + .sbss 合计约 9.6KB，
# 超出窗口的部分会被链接器丢弃，报 "discards section .sdata.xxx / Total discarded sections
# size is 4409 bytes"。
# -msmall-data-limit=0 表示不再把小变量塞进 gp 窗口，全部改用绝对寻址放进 .data/.bss，
# 代价只是极少量指令变长（对功能无任何影响）。
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -msmall-data-limit=0")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -msmall-data-limit=0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.2.3")
project(xiaozhi)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=stringop-overflow" APPEND)

# 同上 -msmall-data-limit=0，再走一遍 idf 编译选项属性通道（双保险，确保所有组件都收到）
idf_build_set_property(COMPILE_OPTIONS "-msmall-data-limit=0" APPEND)
